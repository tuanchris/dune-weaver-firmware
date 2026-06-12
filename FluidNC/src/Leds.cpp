// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Leds.h"

#include "Machine/MachineConfig.h"
#include "Settings.h"
#include "Serial.h"  // allChannels

#include <driver/rmt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>
#include <cstring>

namespace {
    // Step engines allocate RMT channels from 0 upward (esp32/rmt_engine.c),
    // so we take the highest one.
    constexpr rmt_channel_t LED_RMT_CHANNEL = RMT_CHANNEL_7;

    // WS2812 bit timing in 25 ns ticks (80 MHz APB / clk_div 2)
    constexpr rmt_item32_t WS2812_ZERO = {{{ 16, 1, 34, 0 }}};  // 0.4 us high, 0.85 us low
    constexpr rmt_item32_t WS2812_ONE  = {{{ 32, 1, 18, 0 }}};  // 0.8 us high, 0.45 us low

    enum_opt_t ledEffects = {
        { "off", 0 },
        { "static", 1 },
        { "rainbow", 2 },
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
}

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
    memset(_pixels, 0, _num_leds * 3);

    if (!_effect) {
        _effect     = new EnumSetting("LED effect", EXTENDED, WG, NULL, "LED/Effect", EFFECT_RAINBOW, &ledEffects);
        _color      = new StringSetting("LED static color RRGGBB", EXTENDED, WG, NULL, "LED/Color", "FFB060", 0, 7);
        _brightness = new IntSetting("LED brightness", EXTENDED, WG, NULL, "LED/Brightness", 40, 0, 255);
        _speed      = new IntSetting("LED effect speed", EXTENDED, WG, NULL, "LED/Speed", 50, 1, 255);
    }

    log_info("leds: " << _num_leds << " WS2812 on pin " << _data_pin.name() << " order " << _color_order);

    _ready = true;
    allChannels.registration(this);
}

void Leds::deinit() {
    if (_ready) {
        allChannels.deregistration(this);
        rmt_driver_uninstall(LED_RMT_CHANNEL);
        delete[] _pixels;
        _pixels = nullptr;
        _ready  = false;
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

void Leds::render() {
    // Skip the frame if the previous transmission is still in flight;
    // the pixel buffer must not change while the translator reads it.
    if (rmt_wait_tx_done(LED_RMT_CHANNEL, pdMS_TO_TICKS(2)) != ESP_OK) {
        return;
    }

    int     effect     = _effect->get();
    uint8_t brightness = static_cast<uint8_t>(_brightness->get());

    switch (effect) {
        case EFFECT_RAINBOW:
            for (int i = 0; i < _num_leds; i++) {
                uint8_t r, g, b;
                wheel(static_cast<uint8_t>((_phase >> 8) + (i * 256) / _num_leds), r, g, b);
                setPixel(i, r, g, b, brightness);
            }
            break;
        case EFFECT_STATIC: {
            const char* hex = _color->get();
            if (*hex == '#') {
                ++hex;
            }
            uint32_t rgb = strtoul(hex, NULL, 16);
            for (int i = 0; i < _num_leds; i++) {
                setPixel(i, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, brightness);
            }
            break;
        }
        default:  // EFFECT_OFF
            memset(_pixels, 0, _num_leds * 3);
            break;
    }

    rmt_write_sample(LED_RMT_CHANNEL, _pixels, _num_leds * 3, false);
    // The >= frame_ms gap to the next frame doubles as the WS2812 latch time.
}

Error Leds::pollLine(char* line) {
    if (_ready) {
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
