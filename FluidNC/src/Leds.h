// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Leds drives a WS2812/NeoPixel strip from a GPIO via the RMT peripheral.

  Config:
    leds:
      data_pin: gpio.32
      num_leds: 60
      color_order: GRB     # any permutation of R, G, B
      frame_ms: 33         # animation tick, ~30 fps

  Runtime control is via NVS-persisted settings, so the strip remembers
  its state across power cycles and can be driven from serial, WebUI,
  telnet, or macros:
    $LED/Effect=off|static|rainbow
    $LED/Color=RRGGBB      (static effect color, hex)
    $LED/Brightness=0..255
    $LED/Speed=1..255      (rainbow cycle speed)

  Optional machine-state hooks override $LED/Effect automatically:
    $LED/RunEffect=none|off|static|rainbow   while Run/Jog/Homing
    $LED/IdleEffect=none|off|static|rainbow  while Idle/Hold
  "none" (the default) leaves the strip fully manual.  The module
  receives its own auto-reports (like Status_Outputs) to track state.

  Like Status_Outputs, this is a Channel + ConfigurableModule; the
  channel-polling task calls pollLine() continuously (even while a job
  is running), which is what paces the animation frames.

  The RMT translator API is used so memory cost is 3 bytes per pixel.
  We claim RMT_CHANNEL_7 because the RMT step engine allocates channels
  from 0 upward; on I2S-stepping boards (e.g. MKS-DLC32) RMT is unused.
*/

#include "Config.h"
#include "Module.h"
#include "Channel.h"
#include "Pin.h"

#include <string>

class IntSetting;
class EnumSetting;
class StringSetting;

class Leds : public Channel, public ConfigurableModule {
public:
    Leds(const char* name) : Channel(name), ConfigurableModule(name) {}

    Leds(const Leds&)            = delete;
    Leds(Leds&&)                 = delete;
    Leds& operator=(const Leds&) = delete;
    Leds& operator=(Leds&&)      = delete;

    virtual ~Leds() = default;

    void init() override;
    void deinit() override;

    // Channel interface; receives status reports, produces no input
    size_t write(uint8_t data) override;
    Error  pollLine(char* line) override;
    void   flushRx() override {}
    bool   lineComplete(char*, char) override { return false; }
    size_t timedReadBytes(char* buffer, size_t length, TickType_t timeout) override { return 0; }

    // Configuration handlers
    void validate() override {}
    void afterParse() override;
    void group(Configuration::HandlerBase& handler) override {
        handler.item("data_pin", _data_pin);
        handler.item("num_leds", _num_leds, 1, 300);
        handler.item("color_order", _color_order);
        handler.item("frame_ms", _frame_ms, 10, 1000);
    }

private:
    static constexpr int EFFECT_OFF     = 0;
    static constexpr int EFFECT_STATIC  = 1;
    static constexpr int EFFECT_RAINBOW = 2;
    static constexpr int EFFECT_NONE    = 3;  // RunEffect/IdleEffect: no override

    void render();
    void parse_state_report();
    void setPixel(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

    static void wheel(uint8_t pos, uint8_t& r, uint8_t& g, uint8_t& b);

    // Configuration
    Pin         _data_pin;
    int         _num_leds    = 30;
    std::string _color_order = "GRB";
    int         _frame_ms    = 33;

    // Runtime settings ($LED/...)
    EnumSetting*   _effect      = nullptr;
    StringSetting* _color       = nullptr;
    IntSetting*    _brightness  = nullptr;
    IntSetting*    _speed       = nullptr;
    EnumSetting*   _run_effect  = nullptr;
    EnumSetting*   _idle_effect = nullptr;

    // Machine-state tracking (poll task only: reports arrive via our
    // own autoReport, which runs inside pollLine)
    std::string _report;                 // partial report line being received
    int         _auto_effect = -1;       // -1: no override; else EFFECT_*

    // Driver state
    bool     _ready = false;
    uint8_t* _pixels = nullptr;  // 3 bytes per LED, in wire order
    int      _ri = 1, _gi = 0, _bi = 2;  // RGB -> wire-order byte offsets (GRB default)
    uint16_t _phase      = 0;  // rainbow hue phase, 8.8 fixed point
    uint32_t _last_frame = 0;
};
