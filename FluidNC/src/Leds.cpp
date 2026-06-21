// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Leds.h"

#include "Machine/MachineConfig.h"
#include "Settings.h"
#include "Serial.h"  // allChannels
#include "Types.h"   // state_is, State
#include "Kinematics/ThetaRho.h"  // ballAngle() for the 'ball' effect

#include <driver/rmt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
    // Step engines allocate RMT channels from 0 upward (esp32/rmt_engine.c),
    // so we take the highest one.
    constexpr rmt_channel_t LED_RMT_CHANNEL = RMT_CHANNEL_7;

    // WS2812 bit timing in 25 ns ticks (80 MHz APB / clk_div 2)
    constexpr rmt_item32_t WS2812_ZERO = {{{ 16, 1, 34, 0 }}};  // 0.4 us high, 0.85 us low
    constexpr rmt_item32_t WS2812_ONE  = {{{ 32, 1, 18, 0 }}};  // 0.8 us high, 0.45 us low

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
    // Same list plus "none" (no override) for the state hooks.
    enum_opt_t ledHookEffects = {
        { "off", 0 },        { "static", 1 },      { "rainbow", 2 },      { "breathe", 3 },
        { "colorloop", 4 },  { "theater", 5 },     { "scan", 6 },         { "running", 7 },
        { "sine", 8 },       { "gradient", 9 },    { "sinelon", 10 },     { "twinkle", 11 },
        { "sparkle", 12 },   { "fire", 13 },       { "candle", 14 },      { "meteor", 15 },
        { "bouncing", 16 },  { "wipe", 17 },       { "dualscan", 18 },    { "juggle", 19 },
        { "multicomet", 20 },{ "glitter", 21 },    { "dissolve", 22 },    { "ripple", 23 },
        { "drip", 24 },      { "lightning", 25 },  { "fireworks", 26 },   { "plasma", 27 },
        { "heartbeat", 28 }, { "strobe", 29 },     { "police", 30 },      { "chase", 31 },
        { "railway", 32 },   { "pacifica", 33 }, { "aurora", 34 },      { "pride", 35 },
        { "colorwaves", 36 },{ "bpm", 37 },      { "ball", 38 },        { "none", 255 },
    };

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

    // RMT translator: expands pixel bytes into RMT items on demand
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
}

Leds* Leds::_instance = nullptr;

void Leds::afterParse() {
    // Reduce the color order string to byte offsets in wire order
    if (_color_order.length() == 3) {
        for (int i = 0; i < 3; i++) {
            switch (_color_order[i]) {
                case 'R':
                case 'r':
                    _ri = i;
                    break;
                case 'G':
                case 'g':
                    _gi = i;
                    break;
                case 'B':
                case 'b':
                    _bi = i;
                    break;
            }
        }
    }
    bool distinct = _ri != _gi && _gi != _bi && _ri != _bi;
    if (!distinct) {
        log_warn("leds: bad color_order '" << _color_order << "', using GRB");
        _ri = 1;
        _gi = 0;
        _bi = 2;
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
    cfg.mem_block_num           = 1;
    cfg.tx_config.loop_en       = false;
    cfg.tx_config.carrier_en    = false;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level    = RMT_IDLE_LEVEL_LOW;

    if (rmt_config(&cfg) != ESP_OK || rmt_driver_install(cfg.channel, 0, 0) != ESP_OK ||
        rmt_translator_init(cfg.channel, ws2812_translate) != ESP_OK) {
        log_error("leds: RMT init failed");
        return;
    }

    _pixels = new uint8_t[_num_leds * 3];
    _fb     = new uint8_t[_num_leds * 3];
    _heat   = new uint8_t[_num_leds];
    memset(_pixels, 0, _num_leds * 3);
    memset(_fb, 0, _num_leds * 3);
    memset(_heat, 0, _num_leds);
    _rng ^= now_ms() * 2654435761U;  // seed the effect RNG

    if (!_effect) {
        _effect      = new EnumSetting("LED effect", EXTENDED, WG, NULL, "LED/Effect", EFFECT_RAINBOW, &ledEffects);
        _palette     = new EnumSetting("LED palette", EXTENDED, WG, NULL, "LED/Palette", 0, &ledPalettes);
        _color       = new StringSetting("LED primary color RRGGBB", EXTENDED, WG, NULL, "LED/Color", "FFB060", 0, 7);
        _color2      = new StringSetting("LED secondary color RRGGBB", EXTENDED, WG, NULL, "LED/Color2", "0040FF", 0, 7);
        _brightness  = new IntSetting("LED brightness", EXTENDED, WG, NULL, "LED/Brightness", 40, 0, 255);
        _speed       = new IntSetting("LED effect speed", EXTENDED, WG, NULL, "LED/Speed", 50, 1, 255);
        _run_effect  = new EnumSetting("LED effect while running", EXTENDED, WG, NULL, "LED/RunEffect", EFFECT_NONE, &ledHookEffects);
        _idle_effect = new EnumSetting("LED effect while idle", EXTENDED, WG, NULL, "LED/IdleEffect", EFFECT_NONE, &ledHookEffects);
        _direction   = new EnumSetting("LED ball direction", EXTENDED, WG, NULL, "LED/Direction", DIR_CW, &ledDirections);
        _align       = new IntSetting("LED ball alignment, degrees", EXTENDED, WG, NULL, "LED/Align", 0, 0, 359);
        _ballsize    = new IntSetting("LED ball glow size, LEDs", EXTENDED, WG, NULL, "LED/BallSize", 3, 1, 200);
    }

    log_info("leds: " << _num_leds << " WS2812 on pin " << _data_pin.name() << " order " << _color_order);

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
        if (idle) {
            return _effect->setStringValue(value);
        }
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
}

void Leds::setPixel(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    uint16_t scale         = brightness + 1;
    uint8_t* p             = &_pixels[index * 3];
    p[_ri]                 = (r * scale) >> 8;
    p[_gi]                 = (g * scale) >> 8;
    p[_bi]                 = (b * scale) >> 8;
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

// ---- effect rendering ----------------------------------------------------

// Fill _fb (logical R,G,B, full 0..255 range) for the given effect.
// Master brightness is applied later in commit().  `speed` is 1..255.
void Leds::renderEffect(int effect, uint8_t speed) {
    // Reset persistent state whenever the effect changes.
    if (effect != _last_effect) {
        _last_effect = effect;
        memset(_fb, 0, _num_leds * 3);
        memset(_heat, 0, _num_leds);
        _candle = 200;
        _aux    = 0;
        for (int k = 0; k < kBalls; k++) {
            _ball_pos[k] = 0.0f;
            _ball_vel[k] = 4.4f - 0.5f * k;  // staggered launch
        }
        _ball_track = -1.0f;  // 'ball' effect snaps to the current angle on (re)entry
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

        case EFFECT_RAINBOW:
            for (int i = 0; i < _num_leds; i++) {
                palColor(static_cast<uint8_t>(hi + (i * 256) / _num_leds), r, g, b);
                setFb(i, r, g, b);
            }
            break;

        case EFFECT_BREATHE: {
            primaryColor(r, g, b);
            uint16_t lvl = 16 + ((sin8(hi) * 239) >> 8);  // 16..255, never fully off
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * lvl) >> 8, (g * lvl) >> 8, (b * lvl) >> 8);
            }
            break;
        }

        case EFFECT_COLORLOOP:
            palColor(hi, r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, r, g, b);
            }
            break;

        case EFFECT_THEATER: {
            primaryColor(r, g, b);
            int off = (_phase >> 9) % 3;
            for (int i = 0; i < _num_leds; i++) {
                if (i % 3 == off) {
                    setFb(i, r, g, b);
                } else {
                    setFb(i, 0, 0, 0);
                }
            }
            break;
        }

        case EFFECT_SCAN: {  // Larson / Cylon, with a short trail
            primaryColor(r, g, b);
            fadeBy(90);
            int pos = scale8(sin8(hi), _num_leds - 1);
            setFb(pos, r, g, b);
            break;
        }

        case EFFECT_RUNNING: {  // one sine peak of the primary color, sliding
            primaryColor(r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t s = sin8(static_cast<uint8_t>(i * (256 / _num_leds) - hi));
                setFb(i, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8);
            }
            break;
        }

        case EFFECT_SINE: {  // two faster sine bands of the primary color
            primaryColor(r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t s = sin8(static_cast<uint8_t>(i * (512 / _num_leds) + hi));
                setFb(i, (r * s) >> 8, (g * s) >> 8, (b * s) >> 8);
            }
            break;
        }

        case EFFECT_GRADIENT: {  // scrolling blend between color and color2
            uint8_t r1, g1, b1, r2, g2, b2;
            primaryColor(r1, g1, b1);
            secondaryColor(r2, g2, b2);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t t   = static_cast<uint8_t>((i * 256) / _num_leds + hi);
                uint8_t tri = t < 128 ? t * 2 : (255 - t) * 2;  // triangle so the ring loops
                setFb(i, lerp8(r1, r2, tri), lerp8(g1, g2, tri), lerp8(b1, b2, tri));
            }
            break;
        }

        case EFFECT_SINELON: {  // moving dot, fading trail, shifting hue
            fadeBy(40);
            int pos = scale8(sin8(hi), _num_leds - 1);
            palColor(static_cast<uint8_t>(_phase >> 7), r, g, b);
            addFb(pos, r, g, b);
            break;
        }

        case EFFECT_TWINKLE: {  // random pixels light then fade
            fadeBy(28);
            if (random8() < speed) {
                palColor(random8(), r, g, b);
                setFb(random8(_num_leds), r, g, b);
            }
            break;
        }

        case EFFECT_SPARKLE: {  // dim base color + white flashes
            primaryColor(r, g, b);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * 40) >> 8, (g * 40) >> 8, (b * 40) >> 8);
            }
            setFb(random8(_num_leds), 255, 255, 255);
            break;
        }

        case EFFECT_FIRE: {  // Fire2012 on a 1D strip
            const int cooling  = 55;
            const int sparking = 120;
            for (int i = 0; i < _num_leds; i++) {
                _heat[i] = qsub8(_heat[i], random8((cooling * 10) / _num_leds + 2));
            }
            for (int k = _num_leds - 1; k >= 2; k--) {
                _heat[k] = (_heat[k - 1] + _heat[k - 2] + _heat[k - 2]) / 3;
            }
            if (random8() < sparking) {
                int y    = random8(7);
                _heat[y] = qadd8(_heat[y], random8(95) + 160);
            }
            for (int i = 0; i < _num_leds; i++) {
                heatColor(_heat[i], r, g, b);
                setFb(i, r, g, b);
            }
            break;
        }

        case EFFECT_CANDLE: {  // warm global flicker on the primary color
            primaryColor(r, g, b);
            uint8_t target = 100 + random8(155);
            _candle        = (_candle * 7 + target) >> 3;  // smooth toward target
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r * _candle) >> 8, (g * _candle) >> 8, (b * _candle) >> 8);
            }
            break;
        }

        case EFFECT_METEOR: {  // head sweeping the ring, random-decay trail
            primaryColor(r, g, b);
            fadeBy(40 + random8(40));
            int pos = (_phase >> 8) % _num_leds;
            setFb(pos, r, g, b);
            break;
        }

        case EFFECT_BOUNCING: {  // a few balls under gravity
            primaryColor(r, g, b);
            fadeBy(120);
            float dt    = (_frame_ms / 1000.0f) * (speed / 50.0f);
            float gravy = 9.8f;
            for (int k = 0; k < kBalls; k++) {
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

        case EFFECT_WIPE: {  // color-wipe from one end, alternating Color/Color2
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            int span = (_phase >> 7) % (2 * _num_leds);
            for (int i = 0; i < _num_leds; i++) {
                if (span < _num_leds) {
                    if (i <= span) {
                        setFb(i, r, g, b);
                    } else {
                        setFb(i, r2, g2, b2);
                    }
                } else {
                    if (i <= span - _num_leds) {
                        setFb(i, r2, g2, b2);
                    } else {
                        setFb(i, r, g, b);
                    }
                }
            }
            break;
        }

        case EFFECT_DUALSCAN: {  // two dots sweeping toward/past each other
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            fadeBy(80);
            int pos = scale8(sin8(hi), _num_leds - 1);
            addFb(pos, r, g, b);
            addFb(_num_leds - 1 - pos, r2, g2, b2);
            break;
        }

        case EFFECT_JUGGLE: {  // N sine dots of different colors, fading trails
            fadeBy(40);
            for (int k = 0; k < 3; k++) {
                int pos = scale8(sin8(static_cast<uint8_t>((_phase >> 8) * (k + 2))), _num_leds - 1);
                palColor(static_cast<uint8_t>(k * 85 + hi), r, g, b);
                addFb(pos, r, g, b);
            }
            break;
        }

        case EFFECT_MULTICOMET: {  // several comets chasing at different speeds
            fadeBy(48);
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
            if (random8() < 60) {
                setFb(random8(_num_leds), 255, 255, 255);
            }
            break;
        }

        case EFFECT_DISSOLVE: {  // pixels randomly flip Color<->Color2, then reverse
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            int converts = _num_leds / 12 + 1;
            for (int n = 0; n < converts; n++) {
                _heat[random8(_num_leds)] = _aux;  // _aux is the current target (0/1)
            }
            int remaining = 0;
            for (int i = 0; i < _num_leds; i++) {
                if (_heat[i] != _aux) {
                    remaining++;
                }
                if (_heat[i]) {
                    setFb(i, r2, g2, b2);
                } else {
                    setFb(i, r, g, b);
                }
            }
            if (remaining == 0) {
                _aux ^= 1;  // fully converted: dissolve back the other way
            }
            break;
        }

        case EFFECT_RIPPLE: {  // expanding rings from random centers
            fadeBy(40);
            float step = 0.5f * (speed / 50.0f);
            for (int k = 0; k < kBalls; k++) {
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

        case EFFECT_DRIP: {  // droplets fall and splash
            primaryColor(r, g, b);
            fadeBy(60);
            float accel = 0.02f * (speed / 50.0f);
            for (int k = 0; k < kBalls; k++) {
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

        case EFFECT_LIGHTNING: {  // random bright strikes over darkness
            fadeBy(120);
            if (random8() < 12) {
                int start = random8(_num_leds);
                int len   = 2 + random8(_num_leds / 3);
                for (int j = 0; j < len; j++) {
                    setFb((start + j) % _num_leds, 255, 255, 255);
                }
            }
            break;
        }

        case EFFECT_FIREWORKS: {  // random colored bursts that fade
            fadeBy(48);
            if (random8() < 50) {
                int center = random8(_num_leds);
                palColor(random8(), r, g, b);
                addFb(center, r, g, b);
                addFb((center + 1) % _num_leds, r >> 1, g >> 1, b >> 1);
                addFb((center - 1 + _num_leds) % _num_leds, r >> 1, g >> 1, b >> 1);
            }
            break;
        }

        case EFFECT_PLASMA: {  // interfering sines mapped through the color wheel
            for (int i = 0; i < _num_leds; i++) {
                uint8_t x  = (i * 256) / _num_leds;
                uint8_t v1 = sin8(static_cast<uint8_t>(x * 2 + hi));
                uint8_t v2 = sin8(static_cast<uint8_t>(x * 3 - hi * 2));
                palColor(static_cast<uint8_t>((v1 + v2) >> 1), r, g, b);
                setFb(i, r, g, b);
            }
            break;
        }

        case EFFECT_HEARTBEAT: {  // double-pulse brightness (lub-dub)
            primaryColor(r, g, b);
            uint8_t  t = hi;
            uint16_t lvl;
            if (t < 32) {
                lvl = sin8(t * 4);  // lub
            } else if (t < 96) {
                lvl = 24;
            } else if (t < 128) {
                lvl = sin8((t - 96) * 4);  // dub
            } else {
                lvl = 24;
            }
            if (lvl < 24) {
                lvl = 24;
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

        case EFFECT_CHASE: {  // bright dot(s) running over a dim background
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, (r2 * 40) >> 8, (g2 * 40) >> 8, (b2 * 40) >> 8);
            }
            int pos = (_phase >> 8) % _num_leds;
            for (int j = 0; j < 3; j++) {
                setFb((pos + j) % _num_leds, r, g, b);
            }
            break;
        }

        case EFFECT_RAILWAY: {  // alternating two colors with a moving shimmer
            uint8_t r2, g2, b2;
            primaryColor(r, g, b);
            secondaryColor(r2, g2, b2);
            for (int i = 0; i < _num_leds; i++) {
                uint8_t  s     = sin8(static_cast<uint8_t>(i * (256 / _num_leds) - hi));
                uint16_t scale = 100 + ((s * 155) >> 8);
                if (i & 1) {
                    setFb(i, (r * scale) >> 8, (g * scale) >> 8, (b * scale) >> 8);
                } else {
                    setFb(i, (r2 * scale) >> 8, (g2 * scale) >> 8, (b2 * scale) >> 8);
                }
            }
            break;
        }

        case EFFECT_PACIFICA: {  // layered ocean with whitecaps
            for (int i = 0; i < _num_leds; i++) {
                uint8_t a   = sin8(static_cast<uint8_t>(i * 4 + (_phase >> 8)));
                uint8_t c   = sin8(static_cast<uint8_t>(i * 5 - (_phase >> 7)));
                uint8_t d   = sin8(static_cast<uint8_t>(i * 3 + (_phase >> 9)));
                uint8_t idx = (a + c + d) / 3;
                sampleTable(palOcean, idx, r, g, b);
                setFb(i, r, g, b);
                uint8_t cap = sin8(static_cast<uint8_t>(i * 9 - (_phase >> 5)));
                if (cap > 236) {
                    uint8_t w = (cap - 236) * 8;
                    addFb(i, w, w, w);
                }
            }
            break;
        }

        case EFFECT_AURORA: {  // drifting green/purple curtains
            for (int i = 0; i < _num_leds; i++) {
                uint8_t a    = sin8(static_cast<uint8_t>(i * 3 + (_phase >> 8)));
                uint8_t c    = sin8(static_cast<uint8_t>(i * 2 - (_phase >> 9)));
                sampleTable(palAurora, (a + c) / 2, r, g, b);
                uint8_t  w   = sin8(static_cast<uint8_t>(i * 5 + (_phase >> 7)));
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

        case EFFECT_COLORWAVES: {  // pride, but colored from the active palette
            for (int i = 0; i < _num_leds; i++) {
                palColor(static_cast<uint8_t>((_phase >> 8) + i * 3), r, g, b);
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

        case EFFECT_BALL: {  // a smooth glowing blob that tracks the sand ball's
                             // angle over a background colour; size + smoothing tunable
            uint8_t bgr, bgg, bgb;
            secondaryColor(bgr, bgg, bgb);  // background = LED/Color2 (live or setting)
            for (int i = 0; i < _num_leds; i++) {
                setFb(i, bgr, bgg, bgb);
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

            // Smooth motion: glide _ball_track toward the target along the
            // shortest path around the ring.  Speed sets responsiveness
            // (low = smoother/laggier, high = snappier).
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
            primaryColor(r, g, b);  // blob/head colour
            for (int i = 0; i < _num_leds; i++) {
                float d = fabsf(i - posf);
                d       = fminf(d, _num_leds - d);  // circular distance
                float t = 1.0f - d / size;          // linear falloff over `size` LEDs
                if (t <= 0.0f) {
                    continue;  // leaves the background colour
                }
                t = t * t * (3.0f - 2.0f * t);  // smoothstep -> soft blob, anti-aliased
                setFb(i,
                      static_cast<uint8_t>(bgr + (r - bgr) * t),
                      static_cast<uint8_t>(bgg + (g - bgg) * t),
                      static_cast<uint8_t>(bgb + (b - bgb) * t));
            }
            break;
        }

        default:  // EFFECT_OFF
            memset(_fb, 0, _num_leds * 3);
            break;
    }
}

void Leds::commit(uint8_t brightness) {
    for (int i = 0; i < _num_leds; i++) {
        setPixel(i, _fb[i * 3 + 0], _fb[i * 3 + 1], _fb[i * 3 + 2], brightness);
    }
}

void Leds::render() {
    // Skip the frame if the previous transmission is still in flight;
    // the pixel buffer must not change while the translator reads it.
    if (rmt_wait_tx_done(LED_RMT_CHANNEL, pdMS_TO_TICKS(2)) != ESP_OK) {
        return;
    }

    // Still Sands quiet-off wins over everything; then live override beats the
    // state hook beats the persisted setting.
    int effect;
    if (_quiet_off) {
        effect = EFFECT_OFF;
    } else if (!_live_effect.empty()) {
        effect = enumId(ledEffects, _live_effect, _effect->get());
    } else {
        effect = _auto_effect >= 0 ? _auto_effect : _effect->get();
    }
    _cur_palette = _live_palette.empty() ? (_palette ? _palette->get() : 0) : enumId(ledPalettes, _live_palette, 0);

    uint8_t brightness = _live_bright.empty() ? static_cast<uint8_t>(_brightness->get()) : static_cast<uint8_t>(atoi(_live_bright.c_str()));
    uint8_t speed      = _live_speed.empty() ? static_cast<uint8_t>(_speed->get()) : static_cast<uint8_t>(atoi(_live_speed.c_str()));

    renderEffect(effect, speed);
    commit(brightness);

    rmt_write_sample(LED_RMT_CHANNEL, _pixels, _num_leds * 3, false);
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

    int hook = -1;
    if (state == "Run" || state == "Jog" || state == "Home") {
        hook = _run_effect ? _run_effect->get() : EFFECT_NONE;
    } else if (state == "Idle" || state.rfind("Hold", 0) == 0) {
        hook = _idle_effect ? _idle_effect->get() : EFFECT_NONE;
    } else {
        return;  // Alarm/Door/etc: leave the strip as it is
    }
    _auto_effect = hook == EFFECT_NONE ? -1 : hook;
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
            _phase += _speed->get() * 8;
            render();
        }
    }
    return Error::NoData;
}

// Configuration registration
namespace {
    ConfigurableModuleFactory::InstanceBuilder<Leds> registration("leds");
}
