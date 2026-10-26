// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Leds.h"

#include "Machine/MachineConfig.h"
#include "Settings.h"
#include "Serial.h"  // allChannels
#include "Types.h"   // state_is, State
#include "Kinematics/ThetaRho.h"  // ballAngle() for the 'ball' effect

#include <driver/rmt.h>
#include <esp_intr_alloc.h>  // ESP_INTR_FLAG_IRAM
#include <soc/soc_caps.h>  // SOC_RMT_TX_CANDIDATES_PER_GROUP
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>  // std::nothrow (resize)

namespace {
    // Step engines allocate RMT channels from 0 upward (esp32/rmt_engine.c),
    // so we take the highest one that can transmit. The ESP32 can transmit on
    // all 8 channels; the ESP32-S3 also has 8 but only 0-3 are TX-capable
    // (4-7 are receive-only), and rmt_config() rejects an RX channel in
    // RMT_MODE_TX - a silent "leds: RMT init failed" with no other symptom.
    //
    // Two memory blocks, so the channel starts two below the top (it borrows
    // the next channel's block).  The refill ISR must rewrite half the buffer
    // before the hardware wraps back into it; with one block that is 32 bits
    // (40 us; 24 bits / 30 us on the S3), and the I2S step ISR runs at a
    // higher level on the same core at ~62 kHz.  Two blocks double the window.
    constexpr int           LED_RMT_BLOCKS  = 2;
    constexpr rmt_channel_t LED_RMT_CHANNEL = (rmt_channel_t)(SOC_RMT_TX_CANDIDATES_PER_GROUP - LED_RMT_BLOCKS);

    // WS2812/WS2815 bit timing, filled in by init() from the RMT channel's
    // actual counter clock instead of hardcoded tick counts. Both the ESP32
    // and the S3 measure 40 MHz here (25 ns per tick), so this changes no
    // timing today - but the S3's RMT has a group divider the ESP32 lacks
    // (rmt_sclk = src / (1 + div_num + a/b)), so the tick period is a
    // per-chip property rather than a constant, and getting it wrong is
    // invisible: pulses come out a clean multiple too long and the strip
    // simply stays dark. The init() log line reports the measured clock.
    rmt_item32_t WS2812_ZERO;  // 0.4 us high, 0.85 us low
    rmt_item32_t WS2812_ONE;   // 0.8 us high, 0.45 us low

    // Effect name <-> id maps (ids must match the EFFECT_* constants and
    // stay stable: they are persisted in NVS).
    enum_opt_t ledEffects = {
        { "off", 0 },        { "static", 1 },      { "rainbow", 2 },      { "breathe", 3 },
        { "colorloop", 4 },  { "theater", 5 },     { "scan", 6 },         { "running", 7 },
        { "sine", 8 },       { "gradient", 9 },    { "sinelon", 10 },     { "twinkle", 11 },
        { "sparkle", 12 },   { "fire", 13 },       { "candle", 14 },      { "meteor", 15 },
        { "bouncing", 16 },  { "wipe", 17 },       { "dualscan", 18 },    { "juggle", 19 },
        { "multicomet", 20 },{ "glitter", 21 },    { "dissolve", 22 },    { "ripple", 23 },
        { "drip", 24 },      { "lightning", 25 },  { "fireworks", 26 },   { "plasma", 27 },
        { "heartbeat", 28 }, { "strobe", 29 },     { "police", 30 },      { "chase", 31 },
        { "railway", 32 },   { "pacifica", 33 },   { "aurora", 34 },      { "pride", 35 },
        { "colorwaves", 36 },{ "bpm", 37 },        { "ball", 38 },
    };
    enum_opt_t ledDirections = {
        { "cw", 0 }, { "ccw", 1 },
    };
    // $LED/White: WLED's auto-white modes (ids = LedWhite::Mode)
    enum_opt_t ledWhiteModes = {
        { "none", LedWhite::None }, { "brighter", LedWhite::Brighter }, { "accurate", LedWhite::Accurate }, { "max", LedWhite::Max },
    };
    // $LED/Order: every valid wire color order (ids = LedWhite::kOrders index)
    enum_opt_t ledOrders = [] {
        enum_opt_t m;
        for (int i = 0; i < LedWhite::kNumOrders; i++) {
            m[LedWhite::kOrders[i]] = i;
        }
        return m;
    }();
    // Same list plus "none" (no override) for the state hooks. Derived from
    // ledEffects so the two can never drift as effects are added.
    enum_opt_t ledHookEffects = [] {
        enum_opt_t m = ledEffects;
        m["none"] = 255;  // Leds::EFFECT_NONE (no override)
        return m;
    }();

    // 16-entry packed-RGB gradient palettes (mostly the classic FastLED
    // tables).  Index 0 ("rainbow") is special-cased to the wheel(), so it
    // needs no table; these back palette ids 1..7.
    const uint32_t palOcean[16]  = { 0x191970, 0x00008B, 0x191970, 0x000080, 0x00008B, 0x0000CD, 0x2E8B57, 0x008080,
                                     0x5F9EA0, 0x0000FF, 0x008B8B, 0x6495ED, 0x7FFFD4, 0x2E8B57, 0x00FFFF, 0x87CEFA };
    const uint32_t palLava[16]   = { 0x000000, 0x800000, 0x000000, 0x800000, 0x8B0000, 0x800000, 0x8B0000, 0x8B0000,
                                     0x8B0000, 0x8B0000, 0xFF0000, 0xFFA500, 0xFFFFFF, 0xFFA500, 0xFF0000, 0x8B0000 };
    const uint32_t palForest[16] = { 0x006400, 0x006400, 0x556B2F, 0x006400, 0x008000, 0x228B22, 0x6B8E23, 0x008000,
                                     0x2E8B57, 0x66CDAA, 0x32CD32, 0x9ACD32, 0x90EE90, 0x7CFC00, 0x66CDAA, 0x228B22 };
    const uint32_t palParty[16]  = { 0x5500AB, 0x84007C, 0xB5004B, 0xE5001B, 0xE81700, 0xB84700, 0xAB7700, 0xABAB00,
                                     0xAB5500, 0xDD2200, 0xF2000E, 0xC2003E, 0x8F0071, 0x5F00A1, 0x2F00D0, 0x0700E9 };
    const uint32_t palCloud[16]  = { 0x0000FF, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B,
                                     0x0000FF, 0x00008B, 0x87CEEB, 0x87CEEB, 0xADD8E6, 0xFFFFFF, 0xADD8E6, 0x87CEEB };
    const uint32_t palHeat[16]   = { 0x000000, 0x330000, 0x660000, 0x990000, 0xCC0000, 0xFF0000, 0xFF3300, 0xFF6600,
                                     0xFF9900, 0xFFCC00, 0xFFFF00, 0xFFFF33, 0xFFFF66, 0xFFFF99, 0xFFFFCC, 0xFFFFFF };
    const uint32_t palSunset[16] = { 0x00008B, 0x4B0082, 0x800080, 0xC71585, 0xFF1493, 0xFF4500, 0xFF8C00, 0xFFD700,
                                     0xFFA500, 0xFF4500, 0xC71585, 0x800080, 0x4B0082, 0x191970, 0x00008B, 0x000033 };
    // Backs palette ids 1..7 (id 0 = rainbow = wheel()).
    const uint32_t* const kPalettes[] = { palOcean, palLava, palForest, palParty, palCloud, palHeat, palSunset };

    // Self-contained gradients used by the pacifica / aurora effects.
    const uint32_t palAurora[16] = { 0x001A0A, 0x003311, 0x004D1A, 0x006622, 0x00994D, 0x00CC66, 0x00FF7F, 0x2E8B57,
                                     0x008080, 0x4B0082, 0x800080, 0x9400D3, 0x4B0082, 0x008080, 0x00994D, 0x003311 };

    enum_opt_t ledPalettes = {
        { "rainbow", 0 }, { "ocean", 1 }, { "lava", 2 }, { "forest", 3 },
        { "party", 4 },   { "cloud", 5 }, { "heat", 6 }, { "sunset", 7 },
    };

    // RMT translator: expands pixel bytes into RMT items on demand.  It runs
    // inside the RMT refill ISR, which is ESP_INTR_FLAG_IRAM (see init()), so
    // it must stay IRAM/DRAM-only: no logging, no virtual calls, no const
    // tables (they land in flash) -- WS2812_ZERO/ONE are non-const for this.
    void IRAM_ATTR ws2812_translate(
        const void* src, rmt_item32_t* dest, size_t src_size, size_t wanted_num, size_t* translated_size, size_t* item_num) {
        if (!src || !dest) {
            *translated_size = 0;
            *item_num        = 0;
            return;
        }
        size_t         bytes = 0;
        size_t         items = 0;
        const uint8_t* data  = static_cast<const uint8_t*>(src);
        while (bytes < src_size && items + 8 <= wanted_num) {
            uint8_t b = data[bytes];
            for (int bit = 7; bit >= 0; bit--) {
                dest[items++] = (b & (1 << bit)) ? WS2812_ONE : WS2812_ZERO;
            }
            bytes++;
        }
        *translated_size = bytes;
        *item_num        = items;
    }

    uint32_t now_ms() {
        return xTaskGetTickCount() * portTICK_PERIOD_MS;
    }

    // Name -> id lookup in an effect/palette map (returns dflt if not found)
    int enumId(const enum_opt_t& m, const std::string& name, int dflt) {
        auto it = m.find(name.c_str());
        return it == m.end() ? dflt : it->second;
    }

    // id -> name.  Returns a string literal from the map, so the pointer stays
    // valid for callers on other tasks (unlike one into a live std::string).
    const char* enumName(const enum_opt_t& m, int id) {
        for (const auto& kv : m) {
            if (kv.second == id) {
                return kv.first;
            }
        }
        return "off";
    }
}

Leds* Leds::_instance = nullptr;

void Leds::afterParse() {
    // Boot only.  Once running, the wire order belongs to $LED/Order, which
    // render() swaps safely (resizing the buffer); a runtime `$/...` write to
    // ANY config item re-runs every afterParse and would otherwise reset it --
    // to a different bytes-per-pixel than the live buffer, for an RGBW override.
    if (_ready) {
        return;
    }
    // Reduce the color order string to wire byte offsets (+ the W slot and
    // 3-vs-4 bytes per pixel for RGBW strips)
    if (!LedWhite::parseOrder(_color_order.c_str(), _order)) {
        log_warn("leds: bad color_order '" << _color_order << "', using GRB");
        _order = LedWhite::Order();
    }
}

void Leds::init() {
    if (!_data_pin.defined()) {
        log_warn("leds: no data_pin configured");
        return;
    }

    _data_pin.setAttr(Pin::Attr::Output);
    auto gpio = _data_pin.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);

    rmt_config_t cfg            = {};
    cfg.rmt_mode                = RMT_MODE_TX;
    cfg.channel                 = LED_RMT_CHANNEL;
    cfg.gpio_num                = static_cast<gpio_num_t>(gpio);
    cfg.clk_div                 = 2;  // 40 MHz -> 25 ns ticks
    cfg.mem_block_num           = LED_RMT_BLOCKS;
    cfg.tx_config.loop_en       = false;
    cfg.tx_config.carrier_en    = false;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level    = RMT_IDLE_LEVEL_LOW;

    // IRAM: the refill ISR must keep running while a flash op has the cache
    // disabled.  The channel holds only 128 items (96 on the S3) of a 1000+ item
    // frame, so a refill held off by an NVS/littlefs op replays stale items
    // mid-frame and the strip shows wrong colors for a frame (flicker).
    if (rmt_config(&cfg) != ESP_OK || rmt_driver_install(cfg.channel, 0, ESP_INTR_FLAG_IRAM) != ESP_OK ||
        rmt_translator_init(cfg.channel, ws2812_translate) != ESP_OK) {
        log_error("leds: RMT init failed");
        return;
    }

    uint32_t clk_hz = 0;
    if (rmt_get_counter_clock(LED_RMT_CHANNEL, &clk_hz) != ESP_OK || clk_hz == 0) {
        log_error("leds: RMT clock query failed");
        rmt_driver_uninstall(LED_RMT_CHANNEL);
        return;
    }
    auto ticks = [clk_hz](uint32_t ns) -> uint32_t {
        return uint32_t((uint64_t(clk_hz) * ns + 500000000ULL) / 1000000000ULL);
    };
    WS2812_ZERO.duration0 = ticks(400);
    WS2812_ZERO.level0    = 1;
    WS2812_ZERO.duration1 = ticks(850);
    WS2812_ZERO.level1    = 0;
    WS2812_ONE.duration0  = ticks(800);
    WS2812_ONE.level0     = 1;
    WS2812_ONE.duration1  = ticks(450);
    WS2812_ONE.level1     = 0;
    log_debug("leds: RMT " << clk_hz << " Hz, 0/1 high ticks " << ticks(400) << "/" << ticks(800));

    _rng ^= now_ms() * 2654435761U;  // seed the effect RNG

    if (!_effect) {
        // holdOk: LEDs are decoration, never motion -- writable while paused
        // (and, via /sand_led + Leds::setLive, even mid-pattern).
        _effect      = holdOk(new EnumSetting("LED effect", EXTENDED, WG, NULL, "LED/Effect", EFFECT_RAINBOW, &ledEffects));
        _palette     = holdOk(new EnumSetting("LED palette", EXTENDED, WG, NULL, "LED/Palette", 0, &ledPalettes));
        _color       = holdOk(new StringSetting("LED primary color RRGGBB", EXTENDED, WG, NULL, "LED/Color", "FFB060", 0, 7));
        _color2      = holdOk(new StringSetting("LED secondary color RRGGBB", EXTENDED, WG, NULL, "LED/Color2", "0040FF", 0, 7));
        _brightness  = holdOk(new IntSetting("LED brightness", EXTENDED, WG, NULL, "LED/Brightness", 40, 0, 255));
        _speed       = holdOk(new IntSetting("LED effect speed", EXTENDED, WG, NULL, "LED/Speed", 50, 1, 255));
        _intensity   = holdOk(new IntSetting("LED effect intensity", EXTENDED, WG, NULL, "LED/Intensity", 128, 0, 255));
        _white       = holdOk(new EnumSetting("LED RGBW auto-white mode", EXTENDED, WG, NULL, "LED/White", LedWhite::Accurate, &ledWhiteModes));
        _run_effect  = holdOk(new EnumSetting("LED effect while running", EXTENDED, WG, NULL, "LED/RunEffect", EFFECT_NONE, &ledHookEffects));
        _idle_effect = holdOk(new EnumSetting("LED effect while idle", EXTENDED, WG, NULL, "LED/IdleEffect", EFFECT_NONE, &ledHookEffects));
        _direction   = holdOk(new EnumSetting("LED ball direction", EXTENDED, WG, NULL, "LED/Direction", DIR_CW, &ledDirections));
        _align       = holdOk(new IntSetting("LED ball alignment, degrees", EXTENDED, WG, NULL, "LED/Align", 0, 0, 359));
        _ballsize    = holdOk(new IntSetting("LED ball glow size, LEDs", EXTENDED, WG, NULL, "LED/BallSize", 3, 1, 200));
        _ballbg      = holdOk(new EnumSetting("LED ball background effect", EXTENDED, WG, NULL, "LED/BallBg", EFFECT_STATIC, &ledEffects));
        _ballbright  = holdOk(new IntSetting("LED ball blob brightness", EXTENDED, WG, NULL, "LED/BallBright", 255, 0, 255));
        _ballbgbright = holdOk(new IntSetting("LED ball background brightness", EXTENDED, WG, NULL, "LED/BallBgBright", 255, 0, 255));
        // config.yaml's num_leds is the factory default; the app sets the real
        // length here.  render() applies a change on the next frame.
        _count       = holdOk(new IntSetting("LED count", EXTENDED, WG, NULL, "LED/Count", _num_leds, 1, 300));
        // Same for the wire color order: config.yaml's color_order is the
        // default (afterParse already fell back to GRB if it was bad).
        _order_set   = holdOk(new EnumSetting("LED color order", EXTENDED, WG, NULL, "LED/Order",
                                            enumId(ledOrders, _color_order, enumId(ledOrders, "GRB", 0)), &ledOrders));
    }
    // Before resize(), which sizes the wire buffer by the order's bytes per pixel.
    applyOrder(_order_set->get());
    if (!resize(_count->get())) {
        log_error("leds: no memory for " << _count->get() << " LEDs");
        rmt_driver_uninstall(LED_RMT_CHANNEL);
        return;
    }
    // Seed render()'s change detector, or the first frame reads the loaded
    // $LED/Effect as a fresh manual choice and suspends the hook for the boot
    // homing.
    _last_setting_effect = _effect->get();

    log_info("leds: " << _num_leds << (_order.rgbw() ? " RGBW" : " RGB") << " on pin " << _data_pin.name() << " order "
                      << enumName(ledOrders, _applied_order));

    _ready    = true;
    _instance = this;
    allChannels.registration(this);
    setReportInterval(500);  // receive status reports for the state hooks
}

void Leds::deinit() {
    if (_ready) {
        allChannels.deregistration(this);
        rmt_driver_uninstall(LED_RMT_CHANNEL);
        delete[] _pixels;
        delete[] _fb;
        delete[] _heat;
        _pixels = nullptr;
        _fb     = nullptr;
        _heat   = nullptr;
        _ready  = false;
        if (_instance == this) {
            _instance = nullptr;
        }
    }
}

// ---- small helpers -------------------------------------------------------

uint8_t Leds::random8() {
    _rng ^= _rng << 13;
    _rng ^= _rng >> 17;
    _rng ^= _rng << 5;
    return static_cast<uint8_t>(_rng);
}
uint8_t Leds::random8(uint8_t lim) {
    return (static_cast<uint16_t>(random8()) * lim) >> 8;
}
uint8_t Leds::sin8(uint8_t theta) {
    float s = sinf(theta * (2.0f * 3.14159265f / 256.0f));
    return static_cast<uint8_t>(s * 127.5f + 127.5f);
}
uint8_t Leds::scale8(uint8_t v, uint8_t s) {
    return (static_cast<uint16_t>(v) * (s + 1)) >> 8;
}
uint8_t Leds::qadd8(uint8_t a, uint8_t b) {
    int s = a + b;
    return s > 255 ? 255 : s;
}
uint8_t Leds::qsub8(uint8_t a, uint8_t b) {
    int s = a - b;
    return s < 0 ? 0 : s;
}
uint8_t Leds::lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return a + (((static_cast<int>(b) - a) * t) >> 8);
}

void Leds::setFb(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= _num_leds) {
        return;
    }
    _fb[i * 3 + 0] = r;
    _fb[i * 3 + 1] = g;
    _fb[i * 3 + 2] = b;
}
void Leds::addFb(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= _num_leds) {
        return;
    }
    _fb[i * 3 + 0] = qadd8(_fb[i * 3 + 0], r);
    _fb[i * 3 + 1] = qadd8(_fb[i * 3 + 1], g);
    _fb[i * 3 + 2] = qadd8(_fb[i * 3 + 2], b);
}
void Leds::fadeBy(uint8_t amount) {
    uint16_t keep = 256 - amount;
    for (int i = 0; i < _num_leds * 3; i++) {
        _fb[i] = (_fb[i] * keep) >> 8;
    }
}
void Leds::getColor(StringSetting* s, uint8_t& r, uint8_t& g, uint8_t& b) {
    const char* hex = s->get();
    if (*hex == '#') {
        ++hex;
    }
    uint32_t rgb = strtoul(hex, NULL, 16);
    r            = (rgb >> 16) & 0xff;
    g            = (rgb >> 8) & 0xff;
    b            = rgb & 0xff;
}

void Leds::primaryColor(uint8_t& r, uint8_t& g, uint8_t& b) {
    if (_live_color.empty()) {
        getColor(_color, r, g, b);
    } else {
        uint32_t rgb = strtoul(_live_color.c_str(), NULL, 16);
        r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    }
}
void Leds::secondaryColor(uint8_t& r, uint8_t& g, uint8_t& b) {
    if (_live_color2.empty()) {
        getColor(_color2, r, g, b);
    } else {
        uint32_t rgb = strtoul(_live_color2.c_str(), NULL, 16);
        r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    }
}

// Apply a live LED parameter.  Idle: persist to NVS (normal gated setter).
// Running: store an in-memory override (no flash write); flushLive() writes
// it on the return to idle.
Error Leds::setLive(const std::string& key, const std::string& value) {
    bool idle = state_is(State::Idle) || state_is(State::Alarm);

    if (key == "effect") {
        if (enumId(ledEffects, value, -1) < 0) {
            return Error::InvalidStatement;
        }
        // Picking an effect (the app's power button included) outranks the state
        // hook until the machine changes state category.  Without this,
        // $LED/IdleEffect=off makes every "turn the LEDs on" a silent no-op
        // while idle: the setting is written, the hook keeps the strip dark, and
        // the only value that appears to work is brightness 0.
        if (idle) {
            Error err = _effect->setStringValue(value);
            if (err == Error::Ok) {
                _hook.manualChoice();
            }
            return err;
        }
        _hook.manualChoice();
        _live_effect = value;
    } else if (key == "palette") {
        if (enumId(ledPalettes, value, -1) < 0) {
            return Error::InvalidStatement;
        }
        if (idle) {
            return _palette->setStringValue(value);
        }
        _live_palette = value;
    } else if (key == "color") {
        if (idle) {
            return _color->setStringValue(value);
        }
        _live_color = value;
    } else if (key == "color2") {
        if (idle) {
            return _color2->setStringValue(value);
        }
        _live_color2 = value;
    } else if (key == "brightness") {
        int v = atoi(value.c_str());
        if (v < 0 || v > 255) {
            return Error::NumberRange;
        }
        if (idle) {
            return _brightness->setStringValue(value);
        }
        _live_bright = value;
    } else if (key == "speed") {
        int v = atoi(value.c_str());
        if (v < 1 || v > 255) {
            return Error::NumberRange;
        }
        if (idle) {
            return _speed->setStringValue(value);
        }
        _live_speed = value;
    } else if (key == "intensity") {
        int v = atoi(value.c_str());
        if (v < 0 || v > 255) {
            return Error::NumberRange;
        }
        if (idle) {
            return _intensity->setStringValue(value);
        }
        _live_intensity = value;
    } else if (key == "direction") {
        if (enumId(ledDirections, value, -1) < 0) {
            return Error::InvalidStatement;  // expects cw|ccw
        }
        if (idle) {
            return _direction->setStringValue(value);
        }
        _live_direction = value;
    } else if (key == "align") {
        int v = atoi(value.c_str());
        if (v < 0 || v > 359) {
            return Error::NumberRange;
        }
        if (idle) {
            return _align->setStringValue(value);
        }
        _live_align = value;
    } else if (key == "size") {
        int v = atoi(value.c_str());
        if (v < 1 || v > 200) {
            return Error::NumberRange;
        }
        if (idle) {
            return _ballsize->setStringValue(value);
        }
        _live_ballsize = value;
    } else if (key == "bg") {  // ball background sub-effect
        if (enumId(ledEffects, value, -1) < 0) {
            return Error::InvalidStatement;
        }
        if (idle) {
            return _ballbg->setStringValue(value);
        }
        _live_ballbg = value;
    } else if (key == "fgbright" || key == "bgbright") {  // ball blob / background brightness
        int v = atoi(value.c_str());
        if (v < 0 || v > 255) {
            return Error::NumberRange;
        }
        IntSetting*  setting = (key[0] == 'f') ? _ballbright : _ballbgbright;
        std::string& live    = (key[0] == 'f') ? _live_ballbright : _live_ballbgbright;
        if (idle) {
            return setting->setStringValue(value);
        }
        live = value;
    } else {
        return Error::InvalidStatement;
    }
    return Error::Ok;
}

// Persist any in-memory live overrides to NVS, then clear them so the
// $LED/* settings regain authority.  Called once on the Run->Idle edge.
void Leds::flushLive() {
    if (!_live_effect.empty()) {
        _effect->setStringValue(_live_effect);
        _live_effect.clear();
        // Persisting the override is bookkeeping, not a new choice by the user:
        // keep render()'s change detector from reading it as one, so the state
        // hook for the state we just entered still applies.
        _last_setting_effect = _effect->get();
    }
    if (!_live_palette.empty()) {
        _palette->setStringValue(_live_palette);
        _live_palette.clear();
    }
    if (!_live_color.empty()) {
        _color->setStringValue(_live_color);
        _live_color.clear();
    }
    if (!_live_color2.empty()) {
        _color2->setStringValue(_live_color2);
        _live_color2.clear();
    }
    if (!_live_bright.empty()) {
        _brightness->setStringValue(_live_bright);
        _live_bright.clear();
    }
    if (!_live_speed.empty()) {
        _speed->setStringValue(_live_speed);
        _live_speed.clear();
    }
    if (!_live_intensity.empty()) {
        _intensity->setStringValue(_live_intensity);
        _live_intensity.clear();
    }
    if (!_live_direction.empty()) {
        _direction->setStringValue(_live_direction);
        _live_direction.clear();
    }
    if (!_live_align.empty()) {
        _align->setStringValue(_live_align);
        _live_align.clear();
    }
    if (!_live_ballsize.empty()) {
        _ballsize->setStringValue(_live_ballsize);
        _live_ballsize.clear();
    }
    if (!_live_ballbg.empty()) {
        _ballbg->setStringValue(_live_ballbg);
        _live_ballbg.clear();
    }
    if (!_live_ballbright.empty()) {
        _ballbright->setStringValue(_live_ballbright);
        _live_ballbright.clear();
    }
    if (!_live_ballbgbright.empty()) {
        _ballbgbright->setStringValue(_live_ballbgbright);
        _live_ballbgbright.clear();
    }
}

// Allocate before freeing, so an OOM keeps the old strip running (render()
// retries next frame).  Caller guarantees no RMT transmission is in flight.
bool Leds::resize(int n) {
    auto* px   = new (std::nothrow) uint8_t[n * _order.bpp]();
    auto* fb   = new (std::nothrow) uint8_t[n * 3]();
    auto* heat = new (std::nothrow) uint8_t[n]();
    if (!px || !fb || !heat) {
        delete[] px;
        delete[] fb;
        delete[] heat;
        return false;
    }
    if (_pixels && n < _num_leds) {  // shorter strip: blank the old length, or the tail stays lit
        memset(_pixels, 0, _num_leds * _order.bpp);
        rmt_write_sample(LED_RMT_CHANNEL, _pixels, _num_leds * _order.bpp, true);
    }
    delete[] _pixels;
    delete[] _fb;
    delete[] _heat;
    _pixels      = px;
    _fb          = fb;
    _heat        = heat;
    _num_leds    = n;
    _last_effect = _last_bg_effect = -1;  // effects restart on the new length
    return true;
}

// Switch the wire color order to $LED/Order id `id`.  3 <-> 4 bytes per pixel
// (RGB <-> RGBW) reallocates the wire buffer -- before freeing the old one, like
// resize(); false = no memory, old order kept (render() retries next frame).
// Caller guarantees no RMT transmission is in flight.
bool Leds::applyOrder(int id) {
    LedWhite::Order o;
    if (!LedWhite::parseOrder(enumName(ledOrders, id), o)) {
        log_warn("leds: unknown $LED/Order id " << id << ", keeping the current order");
        _applied_order = id;  // don't retry every frame
        return true;
    }
    if (_pixels && o.bpp != _order.bpp) {
        auto* px = new (std::nothrow) uint8_t[_num_leds * o.bpp]();
        if (!px) {
            return false;
        }
        delete[] _pixels;
        _pixels = px;
    }
    _order         = o;
    _applied_order = id;
    return true;  // every pixel's wire bytes now differ, so the next commit() resends
}

// Returns true if the pixel's wire bytes changed.
bool Leds::setPixel(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    uint8_t px[4];
    LedWhite::pack(_order, _white_mode, px, r, g, b, brightness);
    uint8_t* dst = &_pixels[index * _order.bpp];
    if (memcmp(dst, px, _order.bpp) == 0) {
        return false;
    }
    memcpy(dst, px, _order.bpp);
    return true;
}

// Classic 256-position color wheel: red -> green -> blue -> red
void Leds::wheel(uint8_t pos, uint8_t& r, uint8_t& g, uint8_t& b) {
    pos = 255 - pos;
    if (pos < 85) {
        r = 255 - pos * 3;
        g = 0;
        b = pos * 3;
    } else if (pos < 170) {
        pos -= 85;
        r = 0;
        g = pos * 3;
        b = 255 - pos * 3;
    } else {
        pos -= 170;
        r = pos * 3;
        g = 255 - pos * 3;
        b = 0;
    }
}

// FastLED-style black-body heat ramp: black -> red -> yellow -> white
void Leds::heatColor(uint8_t heat, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t t192     = scale8(heat, 191);
    uint8_t heatramp = (t192 & 0x3f) << 2;  // 0..252
    if (t192 & 0x80) {
        r = 255;
        g = 255;
        b = heatramp;
    } else if (t192 & 0x40) {
        r = 255;
        g = heatramp;
        b = 0;
    } else {
        r = heatramp;
        g = 0;
        b = 0;
    }
}

void Leds::sampleTable(const uint32_t* pal, uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b) {
    int      e = index >> 4;          // 0..15
    uint8_t  f = (index & 0x0f) << 4;  // blend amount 0..240
    uint32_t c0 = pal[e];
    uint32_t c1 = pal[(e + 1) & 15];
    r = lerp8((c0 >> 16) & 0xff, (c1 >> 16) & 0xff, f);
    g = lerp8((c0 >> 8) & 0xff, (c1 >> 8) & 0xff, f);
    b = lerp8(c0 & 0xff, c1 & 0xff, f);
}

void Leds::palColor(uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b) {
    int p = _cur_palette;  // resolved once per frame in render()
    if (p == 0) {          // rainbow palette is the classic wheel
        wheel(index, r, g, b);
    } else {
        sampleTable(kPalettes[p - 1], index, r, g, b);
    }
}

uint8_t Leds::ixMore(int base) {
    int v = base * _cur_ix / 128;
    return v > 255 ? 255 : v;
}
uint8_t Leds::ixFade(int base) {
    int v = base * 128 / (_cur_ix ? _cur_ix : 1);
    return v > 255 ? 255 : v;
}
int Leds::ixSpawns(int base) {
    int n = 0;
    for (int acc = base * _cur_ix / 128; acc > 0; acc -= 256) {
        n += acc >= 256 || random8() < acc;
    }
    return n;
}

// ---- effect rendering ----------------------------------------------------

// Fill _fb (logical R,G,B, full 0..255 range) for the given effect.
// Master brightness is applied later in commit().  `speed` is 1..255;
// $LED/Intensity reaches the effects through the ix*() helpers (_cur_ix).
// `nested` is true when rendering a background sub-effect for EFFECT_BALL; it
// tracks its own "last effect" so it doesn't thrash the top-level reset.
void Leds::renderEffect(int effect, uint8_t speed, bool nested) {
    // Reset persistent state whenever the (top-level or background) effect changes.
    int& last = nested ? _last_bg_effect : _last_effect;
    if (effect != last) {
        last = effect;
        memset(_fb, 0, _num_leds * 3);
        memset(_heat, 0, _num_leds);
        _candle = 200;
        _aux    = 0;
        _step   = 0;
        for (int k = 0; k < kBalls; k++) {
            _ball_pos[k] = 0.0f;
            _ball_vel[k] = 4.4f - 0.5f * k;  // staggered launch
        }
        if (!nested) {
            _ball_track = -1.0f;  // 'ball' effect snaps to the current angle on (re)entry
        }
    }

    uint8_t hi    = static_cast<uint8_t>(_phase >> 8);  // smooth 0..255 phase
    uint8_t r, g, b;

    switch (effect) {
        case EFFECT_STATIC:
            primaryColor(r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, r, g, b);
            }
            break;

        case EFFECT_RAINBOW: {  // Size (WLED): 16 << (ix/29) hue steps across the ring = 1/16..16 rainbows, 1 at 128
            int span = 16 << (_cur_ix / 29);
            for (int i = 0; i < _num_leds; i++) {
                palColor(static_cast<uint8_t>(hi + (i * span) / _num_leds), r, g, b);
                setFb(i, r, g, b);
            }
            break;
        }

        case EFFECT_BREATHE: {
            primaryColor(r, g, b);
            uint16_t lvl = 16 + ((sin8(hi) * 239) >> 8);  // 16..255, never fully off
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * lvl) >> 8, (g * lvl) >> 8, (b * lvl) >> 8);
            }
            break;
        }

        case EFFECT_COLORLOOP:  // Saturation (WLED): below 128 washes toward white, ~50% at 0
            palColor(hi, r, g, b);
            if (_cur_ix < 128) {
                uint8_t w = 128 - _cur_ix;
                r = lerp8(r, 255, w);
                g = lerp8(g, 255, w);
                b = lerp8(b, 255, w);
            }
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, r, g, b);
            }
            break;

        case EFFECT_THEATER: {  // Gap size (WLED): 1..5 dark LEDs between lit ones, the classic 1-in-3 at 128
            primaryColor(r, g, b);
            int      w    = 2 + ((_cur_ix * _cur_ix + 4096) >> 14);  // 2..6 (3 across 111..168)
            uint16_t tick = _phase >> 9;
            if (tick != _step) {  // ONE LED per tick, like WLED: jumping several per frame aliases (freezes / runs backward)
                _step = tick;
                _aux  = (_aux + 1) % w;
            }
            int off = _aux % w;
            for (int i = 0; i < _num_leds; i++) {
                if (i % w == off) {
                    setFb(i, r, g, b);
                } else {
                    setFb(i, 0, 0, 0);
                }
            }
            break;
        }

        case EFFECT_SCAN: {  // Larson / Cylon bar, short trail; intensity = # of dots (WLED)
            primaryColor(r, g, b);
            fadeBy(90);
            // Bar width grows with intensity: quadratic, so 0..~73 keeps the single
            // Larson dot and 255 reaches ~N/4 (WLED is linear up to ~N/2).
            int size  = 1 + ((_cur_ix * _cur_ix * _num_leds) >> 18);
            int start = sin8(hi) * (_num_leds - size) / 255;
            for (int j = 0; j < size; j++) {
                setFb(start + j, r, g, b);
            }
            break;
        }

        case EFFECT_RUNNING: {  // sine peaks of the primary color, sliding; Wave width (WLED): more, narrower waves as ix rises
            primaryColor(r, g, b);
            int k = (_cur_ix * _cur_ix + 8192) >> 14;  // whole waves per ring (seamless): 1..4, 1 at 128
            if (k < 1) {
                k = 1;  // always a moving wave (0 would just breathe)
            }
            for (int i = 0; i < _num_leds; i++) {
                uint8_t s = sin8(static_cast<uint8_t>((i * 256 * k) / _num_leds - hi));
                setFb(i, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8);
            }
            break;
        }

        case EFFECT_SINE: {  // sine bands of the primary color; Scale (WLED): more, narrower bands as ix rises
            primaryColor(r, g, b);
            int k = (_cur_ix * _cur_ix + 4096) >> 13;  // whole waves per ring (seamless): 1..8, 2 at 128
            if (k < 1) {
                k = 1;  // always a moving band (0 would just breathe)
            }
            for (int i = 0; i < _num_leds; i++) {
                uint8_t s = sin8(static_cast<uint8_t>((i * 256 * k) / _num_leds + hi));
                setFb(i, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8);
            }
            break;
        }

        case EFFECT_GRADIENT: {  // scrolling blend between color and color2; Spread (WLED): wider blend as ix rises
            uint8_t r1, g1, b1, r2, g2, b2;
            primaryColor(r1, g1, b1);
            secondaryColor(r2, g2, b2);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t t   = static_cast<uint8_t>((i * 256) / _num_leds + hi);
                uint8_t tri = ixFade(t < 128 ? t * 2 : (255 - t) * 2);  // triangle so the ring loops; x128/ix (clamped) = Spread
                setFb(i, lerp8(r1, r2, tri), lerp8(g1, g2, tri), lerp8(b1, b2, tri));
            }
            break;
        }

        case EFFECT_SINELON: {  // moving dot, fading trail, shifting hue
            // WLED fade_out(intensity): HIGHER = faster fade = shorter trail, 255 = the
            // bare dot.  Hyperbolic like WLED's; 40 at 128 keeps the original look.
            int f = 5120 / (256 - _cur_ix);
            fadeBy(f > 255 ? 255 : f);
            int pos = sin8(hi) * (_num_leds - 1) / 255;
            palColor(static_cast<uint8_t>(_phase >> 7), r, g, b);
            addFb(pos, r, g, b);
            break;
        }

        case EFFECT_TWINKLE: {  // random pixels light then fade
            // Speed = how fast they fade, intensity = how many (the two used to
            // be one knob: speed gated the spawn rate).  Both defaults (50/128)
            // give the original look.
            int fade = 28 * speed / 50;
            fadeBy(fade < 1 ? 1 : fade > 255 ? 255 : fade);
            for (int n = ixSpawns(50); n > 0; n--) {
                palColor(random8(), r, g, b);
                setFb(random8(_num_leds), r, g, b);
            }
            break;
        }

        case EFFECT_SPARKLE: {  // dim base color + one white spark; WLED: no intensity, speed = how often it jumps
            primaryColor(r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * 40) >> 8, (g * 40) >> 8, (b * 40) >> 8);
            }
            if (_aux == 0) {  // _aux = frames until the spark jumps, _step = its LED
                _step = (random8() * _num_leds) >> 8;
                int f = (10 + (255 - speed) * 2) / _frame_ms;  // WLED's cycle time, 10..520 ms
                _aux  = f > 255 ? 255 : f;
            } else {
                _aux--;
            }
            setFb(_step, 255, 255, 255);
            break;
        }

        case EFFECT_FIRE: {  // Fire2012 on a 1D strip; WLED: speed = cooling, intensity = spark rate
            int cool = ((20 + speed / 3) * 16) / _num_leds + 2;  // WLED's cooling; ~ the old fixed 55 at speed 50
            if (cool > 255) {
                cool = 255;
            }
            for (int i = 0; i < _num_leds; i++) {
                _heat[i] = qsub8(_heat[i], random8(cool));
            }
            for (int k = _num_leds - 1; k >= 2; k--) {
                _heat[k] = (_heat[k - 1] + _heat[k - 2] + _heat[k - 2]) / 3;
            }
            for (int n = ixSpawns(120); n > 0; n--) {
                int y    = random8(_num_leds < 7 ? _num_leds : 7);  // _heat is only _num_leds long
                _heat[y] = qadd8(_heat[y], random8(95) + 160);
            }
            for (int i = 0; i < _num_leds; i++) {
                heatColor(_heat[i], r, g, b);
                setFb(i, r, g, b);
            }
            break;
        }

        case EFFECT_CANDLE: {  // warm global flicker on the primary color; WLED: intensity = depth, speed = flicker rate
            primaryColor(r, g, b);
            uint8_t depth  = ixMore(155);
            uint8_t target = 255 - depth + random8(depth);
            int     sh     = speed > 252 ? 1 : speed > 99 ? 2 : speed > 49 ? 3 : 4;  // WLED's speed steps; 3 at the default 50
            _candle        = (_candle * ((1 << sh) - 1) + target) >> sh;              // smooth toward target
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * _candle) >> 8, (g * _candle) >> 8, (b * _candle) >> 8);
            }
            break;
        }

        case EFFECT_METEOR: {  // head sweeping the ring, random-decay trail
            primaryColor(r, g, b);
            uint8_t decay = ixFade(40);
            fadeBy(qadd8(decay, random8(decay)));
            int pos = (_phase >> 8) % _num_leds;
            setFb(pos, r, g, b);
            break;
        }

        case EFFECT_BOUNCING: {  // balls under gravity; intensity = # of balls (WLED)
            primaryColor(r, g, b);
            fadeBy(120);
            float dt    = (_frame_ms / 1000.0f) * (speed / 50.0f);
            float gravy = 9.8f;
            int   balls = 1 + _cur_ix * kBalls / 256;  // 1..kBalls, 3 at the default 128
            for (int k = 0; k < balls; k++) {
                _ball_vel[k] -= gravy * dt;
                _ball_pos[k] += _ball_vel[k] * dt;
                if (_ball_pos[k] < 0.0f) {
                    _ball_pos[k] = 0.0f;
                    _ball_vel[k] = -_ball_vel[k] * 0.90f;  // lose energy
                    if (_ball_vel[k] < 0.6f) {
                        _ball_vel[k] = 4.4f - 0.5f * k;  // re-launch a dead ball
                    }
                }
                int idx = static_cast<int>(_ball_pos[k] * (_num_leds - 1));
                addFb(idx, r, g, b);
            }
            break;
        }

        case EFFECT_WIPE: {  // color-wipe from one end, alternating Color/Color2; intensity = edge softness (WLED)
            uint8_t c[2][3];
            primaryColor(c[0][0], c[0][1], c[0][2]);
            secondaryColor(c[1][0], c[1][1], c[1][2]);
            // WLED fades the front LED in over (ix+1)/256 of its step; our wipe moves
            // several LEDs a frame, so the soft front is (ix+1)/64 LEDs: hard at 0, ~2 at 128.
            int w    = (_cur_ix + 1) * 2;     // soft-front width, 1/128 LED (>= 2, never 0)
            int len  = 128 * _num_leds + w;   // one pass; the front runs off the end so the last LED finishes
            int p    = _phase % (2u * len);
            int pass = p >= len;              // 0: Color over Color2, 1: Color2 back over Color
            p -= pass * len;
            const uint8_t* nw = c[pass];
            const uint8_t* od = c[pass ^ 1];
            for (int i = 0; i < _num_leds; i++) {
                int d = p - i * 128;  // how far the front is past LED i
                int t = d <= 0 ? 0 : d >= w ? 256 : d * 256 / w;
                setFb(i, od[0] + ((nw[0] - od[0]) * t >> 8), od[1] + ((nw[1] - od[1]) * t >> 8), od[2] + ((nw[2] - od[2]) * t >> 8));
            }
            break;
        }

        case EFFECT_DUALSCAN: {  // two bars sweeping toward/past each other; intensity = # of dots (WLED)
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            fadeBy(80);
            int size  = 1 + ((_cur_ix * _cur_ix * _num_leds) >> 18);  // same curve as scan
            int start = sin8(hi) * (_num_leds - size) / 255;
            for (int j = start; j < start + size; j++) {
                addFb(j, r, g, b);
                addFb(_num_leds - 1 - j, r2, g2, b2);
            }
            break;
        }

        case EFFECT_JUGGLE: {  // N sine dots of different colors, fading trails
            fadeBy(ixFade(40));
            for (int k = 0; k < 3; k++) {
                int pos = scale8(sin8(static_cast<uint8_t>((_phase >> 8) * (k + 2))), _num_leds - 1);
                palColor(static_cast<uint8_t>(k * 85 + hi), r, g, b);
                addFb(pos, r, g, b);
            }
            break;
        }

        case EFFECT_MULTICOMET: {  // several comets chasing at different speeds
            // WLED "Fade": HIGHER = faster fade = shorter tails, 255 = bare heads.
            int f = 6144 / (256 - _cur_ix);  // 48 at 128 = the original look
            fadeBy(f > 255 ? 255 : f);
            for (int k = 0; k < 3; k++) {
                int pos = ((_phase >> 8) * (k + 1) / 2 + k * (_num_leds / 3)) % _num_leds;
                palColor(static_cast<uint8_t>(k * 85), r, g, b);
                addFb(pos, r, g, b);
            }
            break;
        }

        case EFFECT_GLITTER: {  // rainbow base with random white sparks
            for (int i = 0; i < _num_leds; i++) {
                palColor(static_cast<uint8_t>(hi + (i * 256) / _num_leds), r, g, b);
                setFb(i, (r * 180) >> 8, (g * 180) >> 8, (b * 180) >> 8);
            }
            for (int n = ixSpawns(60); n > 0; n--) {
                setFb(random8(_num_leds), 255, 255, 255);
            }
            break;
        }

        case EFFECT_DISSOLVE: {  // pixels randomly flip Color<->Color2; WLED: intensity = dissolve speed, speed = repeat speed
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            int converts = ixSpawns(256 * (_num_leds / 12 + 1));  // _num_leds/12+1 per frame at 128, ~2x at 255, 0 = paused
            for (int n = 0; n < converts; n++) {
                _heat[random8(_num_leds)] = _aux ^ 1;  // target colour: Color2 first, then back
            }
            for (int i = 0; i < _num_leds; i++) {
                if (_heat[i]) {
                    setFb(i, r2, g2, b2);
                } else {
                    setFb(i, r, g, b);
                }
            }
            // WLED reverses every (255 - speed) + 15 of its ~24 ms frames, finished or not.
            if (++_step * _frame_ms > ((255 - speed) + 15) * 24) {
                _aux ^= 1;
                _step = 0;
            }
            break;
        }

        case EFFECT_RIPPLE: {  // expanding rings from random centers; intensity = # of waves (WLED "Wave #")
            fadeBy(40);
            float step  = 0.5f * (speed / 50.0f);
            int   waves = 1 + _cur_ix * kBalls / 256;  // 1..kBalls, 3 at the default 128
            for (int k = 0; k < waves; k++) {
                if (_ball_vel[k] <= 0.0f || _ball_vel[k] > _num_leds / 2.0f) {
                    _ball_pos[k] = random8(_num_leds);
                    _ball_vel[k] = 0.01f;
                }
                int   center = static_cast<int>(_ball_pos[k]);
                int   radius = static_cast<int>(_ball_vel[k]);
                uint8_t fade = 255 - scale8(static_cast<uint8_t>(_ball_vel[k] * 512 / _num_leds), 255);
                palColor(static_cast<uint8_t>(center * 5), r, g, b);
                addFb((center + radius) % _num_leds, (r * fade) >> 8, (g * fade) >> 8, (b * fade) >> 8);
                addFb((center - radius + _num_leds) % _num_leds, (r * fade) >> 8, (g * fade) >> 8, (b * fade) >> 8);
                _ball_vel[k] += step;
            }
            break;
        }

        case EFFECT_DRIP: {  // droplets fall and splash; intensity = # of drips (WLED: 1 + ix/64)
            primaryColor(r, g, b);
            fadeBy(60);
            float accel = 0.02f * (speed / 50.0f);
            int   drips = 1 + (_cur_ix >> 6);  // 1..4 (needs kBalls >= 4), 3 at the default 128
            for (int k = 0; k < drips; k++) {
                _ball_vel[k] += accel;
                _ball_pos[k] -= _ball_vel[k];
                if (_ball_pos[k] <= 0.0f) {
                    setFb(0, 255, 255, 255);  // splash
                    setFb(1, (r * 128) >> 8, (g * 128) >> 8, (b * 128) >> 8);
                    _ball_pos[k] = _num_leds - 1 - random8(_num_leds / 2);  // respawn, staggered
                    _ball_vel[k] = 0.0f;
                }
                addFb(static_cast<int>(_ball_pos[k]), r, g, b);
            }
            break;
        }

        case EFFECT_LIGHTNING: {  // random bright strikes over darkness; WLED: intensity = flashes per strike, speed = shorter gaps
            fadeBy(120);
            if (_aux == 0) {  // between strikes: _step = frames until the next one
                if (_step > 0) {
                    _step--;
                } else {
                    _aux    = 1 + random8(_cur_ix / 20 + 1);  // flashes: 1 at 0, 1-7 at 128, 1-13 at 255
                    _candle = 0;                              // first flash now
                }
            }
            if (_aux && _candle-- == 0) {  // _candle = frames to the next flash
                _aux--;
                _candle   = 1 + random8(3);  // 2-4 frame gaps (~WLED's 50-150 ms)
                int start = random8(_num_leds);
                int len   = 2 + random8(_num_leds / 3);
                for (int j = 0; j < len; j++) {
                    setFb((start + j) % _num_leds, 255, 255, 255);
                }
                if (_aux == 0) {  // strike over: WLED waits random(255 - speed) x 100 ms
                    _step = random8(255 - speed) * 100 / _frame_ms;
                }
            }
            break;
        }

        case EFFECT_FIREWORKS: {  // random colored bursts that fade
            fadeBy(48);
            for (int n = ixSpawns(50); n > 0; n--) {
                int center = random8(_num_leds);
                palColor(random8(), r, g, b);
                addFb(center, r, g, b);
                addFb((center + 1) % _num_leds, r >> 1, g >> 1, b >> 1);
                addFb((center - 1 + _num_leds) % _num_leds, r >> 1, g >> 1, b >> 1);
            }
            break;
        }

        case EFFECT_PLASMA: {  // interfering sines mapped through the color wheel; intensity = brightness
            // WLED Plasma: dark troughs breathe in on a slow beat, deeper at low intensity
            uint8_t dip = scale8(sin8(static_cast<uint8_t>(_phase >> 10)), 255 - _cur_ix);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t x   = (i * 256) / _num_leds;
                uint8_t v1  = sin8(static_cast<uint8_t>(x * 2 + hi));
                uint8_t v2  = sin8(static_cast<uint8_t>(x * 3 - hi * 2));
                uint8_t v   = (v1 + v2) >> 1;
                uint8_t bri = 255 - scale8(dip, 255 - v);  // low palette indexes dim first, as in WLED
                palColor(v, r, g, b);
                setFb(i, scale8(r, bri), scale8(g, bri), scale8(b, bri));
            }
            break;
        }

        case EFFECT_HEARTBEAT: {  // double-pulse brightness (lub-dub); intensity = shorter, sharper beats
            primaryColor(r, g, b);
            uint8_t t = hi;
            uint8_t w = ixFade(32);  // beat length (WLED: faster fade): 32 at 128, 16 at 255
            if (w > 96) {
                w = 96;  // low intensity glows on, but lub must end before dub
            }
            uint16_t lvl = 24;
            if (t < w) {
                lvl = sin8(t * 128 / w);  // lub
            } else if (t >= 96 && t < 96 + w) {
                lvl = sin8((t - 96) * 128 / w);  // dub
            }
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * lvl) >> 8, (g * lvl) >> 8, (b * lvl) >> 8);
            }
            break;
        }

        case EFFECT_STROBE: {  // brief full-strip flashes
            primaryColor(r, g, b);
            bool on = (hi < 32);
            for (int i = 0; i < _num_leds; i++) {
                if (on) {
                    setFb(i, r, g, b);
                } else {
                    setFb(i, 0, 0, 0);
                }
            }
            break;
        }

        case EFFECT_POLICE: {  // alternating red / blue halves
            bool phaseA = (_phase >> 9) & 1;
            int  half   = _num_leds / 2;
            for (int i = 0; i < _num_leds; i++) {
                bool left = i < half;
                if (left == phaseA) {
                    setFb(i, left ? 255 : 0, 0, left ? 0 : 255);
                } else {
                    setFb(i, 0, 0, 0);
                }
            }
            break;
        }

        case EFFECT_CHASE: {  // bright block running over a dim background; intensity = width
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r2 * 40) >> 8, (g2 * 40) >> 8, (b2 * 40) >> 8);
            }
            // WLED Chase "Width": 1..3 LEDs up to 128, then up to ~half the ring at 255
            int w   = _cur_ix <= 128 ? 1 + _cur_ix / 64 : 3 + (_cur_ix - 128) * _num_leds / 256;
            int pos = (_phase >> 8) % _num_leds;
            for (int j = 0; j < w; j++) {
                setFb((pos + j) % _num_leds, r, g, b);
            }
            break;
        }

        case EFFECT_RAILWAY: {  // alternate pixels swap two colors, with a moving shimmer
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            // WLED Railway "Smoothness": each swap cross-fades over ix/256 of its half-cycle; 0 = hard flip
            int     ramp = _cur_ix >> 1;
            int     u    = hi & 127;
            uint8_t pos  = u >= ramp ? 255 : u * 255 / ramp;  // u < ramp implies ramp > 0
            if (hi & 128) {
                pos = 255 - pos;
            }
            for (int i = 0; i < _num_leds; i++) {
                uint8_t  s     = sin8(static_cast<uint8_t>((i * 256) / _num_leds - hi));
                uint16_t scale = 100 + ((s * 155) >> 8);
                uint8_t  t     = (i & 1) ? pos : 255 - pos;
                setFb(i, (lerp8(r2, r, t) * scale) >> 8, (lerp8(g2, g, t) * scale) >> 8, (lerp8(b2, b, t) * scale) >> 8);
            }
            break;
        }

        case EFFECT_PACIFICA: {  // layered ocean with whitecaps; intensity = wave angle
            for (int i = 0; i < _num_leds; i++) {
                int     x   = i * (120 + _cur_ix);  // WLED Pacifica "Angle": per-LED wave step, 248 at 128
                uint8_t a   = sin8(static_cast<uint8_t>(x / 62 + (_phase >> 8)));
                uint8_t c   = sin8(static_cast<uint8_t>(x * 5 / 248 - (_phase >> 7)));
                uint8_t d   = sin8(static_cast<uint8_t>(x * 3 / 248 + (_phase >> 9)));
                uint8_t idx = (a + c + d) / 3;
                sampleTable(palOcean, idx, r, g, b);
                setFb(i, r, g, b);
                uint8_t cap = sin8(static_cast<uint8_t>(x * 9 / 248 - (_phase >> 5)));
                if (cap > 236) {
                    uint8_t w = (cap - 236) * 8;
                    addFb(i, w, w, w);
                }
            }
            break;
        }

        case EFFECT_AURORA: {  // drifting green/purple curtains; intensity = how many curtains
            for (int i = 0; i < _num_leds; i++) {
                int     x    = i * (_cur_ix + 32);  // WLED Aurora wave count: 0.2x..1.8x, 160 per LED at 128
                uint8_t a    = sin8(static_cast<uint8_t>(x * 3 / 160 + (_phase >> 8)));
                uint8_t c    = sin8(static_cast<uint8_t>(x / 80 - (_phase >> 9)));
                sampleTable(palAurora, (a + c) / 2, r, g, b);
                uint8_t  w   = sin8(static_cast<uint8_t>(x / 32 + (_phase >> 7)));
                uint16_t bri = 40 + ((w * 215) >> 8);
                setFb(i, (r * bri) >> 8, (g * bri) >> 8, (b * bri) >> 8);
            }
            break;
        }

        case EFFECT_PRIDE: {  // flowing rainbow with brightness waves
            for (int i = 0; i < _num_leds; i++) {
                wheel(static_cast<uint8_t>((_phase >> 8) + i * 2), r, g, b);
                uint8_t  bw  = sin8(static_cast<uint8_t>(i * 7 + (_phase >> 6)));
                uint16_t bri = 60 + ((bw * 195) >> 8);
                setFb(i, (r * bri) >> 8, (g * bri) >> 8, (b * bri) >> 8);
            }
            break;
        }

        case EFFECT_COLORWAVES: {  // pride, but colored from the active palette; intensity = hue spread
            for (int i = 0; i < _num_leds; i++) {
                // WLED Colorwaves "Hue": palette step per LED, 0 = one color on the whole ring
                palColor(static_cast<uint8_t>((_phase >> 8) + ((i * 3 * _cur_ix) >> 7)), r, g, b);
                uint8_t  bw  = sin8(static_cast<uint8_t>(i * 7 + (_phase >> 6)));
                uint16_t bri = 60 + ((bw * 195) >> 8);
                setFb(i, (r * bri) >> 8, (g * bri) >> 8, (b * bri) >> 8);
            }
            break;
        }

        case EFFECT_BPM: {  // palette sweep pulsed by a beat
            uint8_t beat = sin8(static_cast<uint8_t>(_phase >> 6));
            for (int i = 0; i < _num_leds; i++) {
                palColor(static_cast<uint8_t>((_phase >> 8) + i * 3), r, g, b);
                uint8_t bri = qsub8(beat, static_cast<uint8_t>(i * 2));
                if (bri < 40) {
                    bri = 40;
                }
                setFb(i, (r * bri) >> 8, (g * bri) >> 8, (b * bri) >> 8);
            }
            break;
        }

        case EFFECT_BALL: {  // a smooth blob that tracks the sand ball's angle over
                             // a background (solid colour or an animated sub-effect)
            // --- background ---
            int bg = _live_ballbg.empty() ? (_ballbg ? _ballbg->get() : EFFECT_STATIC)
                                          : enumId(ledEffects, _live_ballbg, EFFECT_STATIC);
            if (bg == EFFECT_BALL) {
                bg = EFFECT_STATIC;  // no recursion
            }
            if (bg == EFFECT_OFF) {
                memset(_fb, 0, _num_leds * 3);
            } else if (bg == EFFECT_STATIC) {
                uint8_t br, bgc, bb;
                secondaryColor(br, bgc, bb);  // solid background = LED/Color2
                for (int i = 0; i < _num_leds; i++) {
                    setFb(i, br, bgc, bb);
                }
            } else {
                renderEffect(bg, speed, true);  // animated sub-effect into _fb
            }
            // background brightness (independent of the blob + master)
            int bgbright = _live_ballbgbright.empty() ? (_ballbgbright ? _ballbgbright->get() : 255)
                                                      : atoi(_live_ballbgbright.c_str());
            if (bgbright < 255) {
                for (int i = 0; i < _num_leds * 3; i++) {
                    _fb[i] = scale8(_fb[i], static_cast<uint8_t>(bgbright));
                }
            }

            float frac = Kinematics::ThetaRho::ballAngle();  // 0..1, or -1 if no kinematics
            if (frac < 0.0f) {
                break;  // no ThetaRho -> just the background
            }
            int dir = _live_direction.empty() ? (_direction ? _direction->get() : DIR_CW)
                                              : enumId(ledDirections, _live_direction, DIR_CW);
            int   align_deg = _live_align.empty() ? (_align ? _align->get() : 0) : atoi(_live_align.c_str());
            float pf        = frac * (dir == DIR_CCW ? -1.0f : 1.0f) + align_deg / 360.0f;
            pf -= floorf(pf);  // wrap into [0,1)

            // Smooth motion: glide _ball_track toward the target along the shortest
            // path around the ring.  Speed sets responsiveness (low = smoother).
            if (_ball_track < 0.0f) {
                _ball_track = pf;  // snap on first frame / (re)entry
            }
            float alpha = speed / 255.0f;
            if (alpha < 0.03f) {
                alpha = 0.03f;
            }
            float delta = pf - _ball_track;
            delta -= roundf(delta);  // shortest wrap into [-0.5, 0.5]
            _ball_track += delta * alpha;
            _ball_track -= floorf(_ball_track);  // back into [0,1)

            float posf = _ball_track * _num_leds;  // sub-pixel position (no LED snapping)
            int   size = _live_ballsize.empty() ? (_ballsize ? _ballsize->get() : 3) : atoi(_live_ballsize.c_str());
            if (size < 1) {
                size = 1;
            }
            // --- blob, with its own brightness, composited over the background ---
            int bright = _live_ballbright.empty() ? (_ballbright ? _ballbright->get() : 255)
                                                  : atoi(_live_ballbright.c_str());
            primaryColor(r, g, b);  // blob colour
            r = scale8(r, static_cast<uint8_t>(bright));
            g = scale8(g, static_cast<uint8_t>(bright));
            b = scale8(b, static_cast<uint8_t>(bright));
            for (int i = 0; i < _num_leds; i++) {
                float d = fabsf(i - posf);
                d       = fminf(d, _num_leds - d);  // circular distance
                float t = 1.0f - d / size;          // linear falloff over `size` LEDs
                if (t <= 0.0f) {
                    continue;  // leaves the background pixel
                }
                t = t * t * (3.0f - 2.0f * t);  // smoothstep -> soft blob, anti-aliased
                uint8_t cr = _fb[i * 3 + 0], cg = _fb[i * 3 + 1], cb = _fb[i * 3 + 2];
                setFb(i,
                      static_cast<uint8_t>(cr + (r - cr) * t),  // blend blob over background
                      static_cast<uint8_t>(cg + (g - cg) * t),
                      static_cast<uint8_t>(cb + (b - cb) * t));
            }
            break;
        }

        default:  // EFFECT_OFF
            memset(_fb, 0, _num_leds * 3);
            break;
    }
}

bool Leds::commit(uint8_t brightness) {
    _white_mode  = static_cast<LedWhite::Mode>(_white ? _white->get() : LedWhite::Accurate);
    bool changed = false;
    for (int i = 0; i < _num_leds; i++) {
        changed |= setPixel(i, _fb[i * 3 + 0], _fb[i * 3 + 1], _fb[i * 3 + 2], brightness);
    }
    return changed;
}

// The state hook in force, or -1 for none.  Suppressed while the user's own
// effect choice stands for this state category (see LedHook).
int Leds::hookEffect() {
    return _hook.effect(_run_effect ? _run_effect->get() : -1, _idle_effect ? _idle_effect->get() : -1);
}

// Still Sands quiet-off wins over everything; then the live override beats the
// state hook, which beats the persisted setting.
int Leds::effectiveEffect() {
    if (_quiet_off) {
        return EFFECT_OFF;
    }
    if (!_live_effect.empty()) {
        return enumId(ledEffects, _live_effect, _effect->get());
    }
    int hook = hookEffect();
    return hook >= 0 ? hook : _effect->get();
}

const char* Leds::activeEffect() {
    return enumName(ledEffects, effectiveEffect());
}

const char* Leds::overrideSource() {
    if (_quiet_off) {
        return "quiet";
    }
    if (!_live_effect.empty()) {
        return nullptr;  // the live value IS the user's own choice
    }
    if (hookEffect() < 0) {
        return nullptr;
    }
    return _hook.cat() == LedHook::CatRun ? "run" : "idle";
}

void Leds::render() {
    // Skip the frame if the previous transmission is still in flight;
    // the pixel buffer must not change while the translator reads it.
    if (rmt_wait_tx_done(LED_RMT_CHANNEL, pdMS_TO_TICKS(2)) != ESP_OK) {
        return;
    }
    if (_count->get() != _num_leds && !resize(_count->get())) {
        return;  // no memory for the new length yet; keep the old frame
    }
    if (_order_set->get() != _applied_order && !applyOrder(_order_set->get())) {
        return;  // no memory for the wider (RGBW) buffer yet; keep the old frame
    }

    // A change to $LED/Effect from any source -- including a bare
    // "$LED/Effect=static" over serial, which never reaches setLive() -- is an
    // explicit choice, so it releases the state hook the same way.
    int setting_effect = _effect->get();
    if (setting_effect != _last_setting_effect) {
        _last_setting_effect = setting_effect;
        _hook.manualChoice();
    }

    int effect   = effectiveEffect();
    _cur_palette = _live_palette.empty() ? (_palette ? _palette->get() : 0) : enumId(ledPalettes, _live_palette, 0);

    uint8_t brightness = _live_bright.empty() ? static_cast<uint8_t>(_brightness->get()) : static_cast<uint8_t>(atoi(_live_bright.c_str()));
    uint8_t speed      = _live_speed.empty() ? static_cast<uint8_t>(_speed->get()) : static_cast<uint8_t>(atoi(_live_speed.c_str()));
    _cur_ix            = _live_intensity.empty() ? static_cast<uint8_t>(_intensity->get()) : static_cast<uint8_t>(atoi(_live_intensity.c_str()));
    _phase += speed * 8;  // the live override too, so the app's speed slider works mid-pattern

    renderEffect(effect, speed);

    // An unchanged frame (static, off) is only resent every kRefreshMs: each
    // send is another chance for a late refill to corrupt it, and the periodic
    // resend still repairs a bad frame or a strip that powered up after us.
    uint32_t now = now_ms();
    if (!commit(brightness) && now - _last_send < kRefreshMs) {
        return;
    }
    _last_send = now;
    rmt_write_sample(LED_RMT_CHANNEL, _pixels, _num_leds * _order.bpp, false);
    // The >= frame_ms gap to the next frame doubles as the WS2812 latch time.
}

// Reports arrive through our own autoReport; collect lines and track
// the machine state, switching the effect when a hook is configured.
size_t Leds::write(uint8_t data) {
    char c = static_cast<char>(data);
    if (c == '\r') {
        return 1;
    }
    if (c == '\n') {
        if (!_report.empty() && _report[0] == '<') {
            parse_state_report();
        }
        _report.clear();
        return 1;
    }
    if (_report.size() < 200) {
        _report += c;
    }
    return 1;
}

void Leds::parse_state_report() {
    // "<State|...>" -> first field
    auto endpos = _report.find_first_of("|>");
    if (endpos == std::string::npos || endpos < 2) {
        return;
    }
    std::string state = _report.substr(1, endpos - 1);

    LedHook::Cat cat;
    if (state == "Run" || state == "Jog" || state == "Home") {
        cat = LedHook::CatRun;
    } else if (state == "Idle" || state.rfind("Hold", 0) == 0) {
        cat = LedHook::CatIdle;
    } else {
        return;  // Alarm/Door/etc: leave the strip as it is
    }
    _hook.reportCat(cat);
}

Error Leds::pollLine(char* line) {
    if (_ready) {
        autoReport();
        // On the Run->Idle edge, persist any live overrides set during the
        // run (flash writes are safe now) and hand authority back to $LED/*.
        bool idle = state_is(State::Idle) || state_is(State::Alarm);
        if (_was_running && idle) {
            flushLive();
        }
        _was_running = !idle;
        uint32_t now = now_ms();
        if (now - _last_frame >= static_cast<uint32_t>(_frame_ms)) {
            _last_frame = now;
            render();
        }
    }
    return Error::NoData;
}

// Configuration registration
namespace {
    ConfigurableModuleFactory::InstanceBuilder<Leds> registration("leds");
}
