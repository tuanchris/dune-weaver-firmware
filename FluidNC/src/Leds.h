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
    $LED/Effect=<name>     (see effect list below)
    $LED/Palette=<name>    (recolors the hue-cycling effects)
    $LED/Color=RRGGBB      (primary color, hex)
    $LED/Color2=RRGGBB     (secondary color, used by 'gradient')
    $LED/Brightness=0..255 (master brightness over every effect)
    $LED/Speed=1..255      (animation speed)
    $LED/Direction=cw|ccw  ('ball' effect: ring winding vs theta)
    $LED/Align=0..359      ('ball' effect: angular offset, degrees)

  Effects:
    off static rainbow breathe colorloop theater scan running sine
    gradient sinelon twinkle sparkle fire candle meteor bouncing
    wipe dualscan juggle multicomet glitter dissolve ripple drip
    lightning fireworks plasma heartbeat strobe police chase railway
    pacifica aurora pride colorwaves bpm ball

  Palettes (rainbow == the classic wheel; the rest are gradient tables):
    rainbow ocean lava forest party cloud heat sunset
  The hue-cycling effects (rainbow, colorloop, sinelon, twinkle, juggle,
  multicomet, glitter, ripple, fireworks, plasma, colorwaves, bpm) draw
  their colors from the active palette via palColor().

  Stateless effects (rainbow, breathe, gradient, ...) recompute every
  pixel each frame.  The animated ones (sinelon, twinkle, fire, ...)
  keep a persistent RGB framebuffer (_fb) that each frame dims via
  fadeBy() before drawing the new head/sparks, which is how trails and
  decay are produced.  fire keeps an extra per-pixel heat buffer.

  Optional machine-state hooks override $LED/Effect automatically:
    $LED/RunEffect=none|<effect>   while Run/Jog/Homing
    $LED/IdleEffect=none|<effect>  while Idle/Hold
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
#include <cstdlib>

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

    // Live control, usable while a pattern is running (unlike the gated
    // $LED/* settings).  key in {effect,palette,color,color2,brightness,
    // speed}.  When idle the value is persisted to NVS; while moving it is
    // applied in memory only (no flash write) and flushed to NVS on the
    // return to idle.  Reached from $Sand/Led and /sand_led.
    static Leds* instance() { return _instance; }
    Error        setLive(const std::string& key, const std::string& value);

    // Still Sands: force the strip off (or back on) during quiet hours, in
    // memory only - no NVS write, works whether idle or running, and overrides
    // every effect/override.  Driven by Playlist on the quiet-hours edge.
    void setQuietOff(bool off) { _quiet_off = off; }
    // Effective values for status reporting (live override or persisted).
    const char*  liveEffect() { return _live_effect.empty() ? nullptr : _live_effect.c_str(); }
    int          liveBrightness() { return _live_bright.empty() ? -1 : atoi(_live_bright.c_str()); }

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
    // Effect ids.  These are persisted in NVS ($LED/Effect, RunEffect,
    // IdleEffect), so existing values must keep their numbers; append
    // new effects at the end.
    static constexpr int EFFECT_OFF       = 0;
    static constexpr int EFFECT_STATIC    = 1;
    static constexpr int EFFECT_RAINBOW   = 2;
    static constexpr int EFFECT_BREATHE   = 3;
    static constexpr int EFFECT_COLORLOOP = 4;
    static constexpr int EFFECT_THEATER   = 5;
    static constexpr int EFFECT_SCAN      = 6;
    static constexpr int EFFECT_RUNNING   = 7;
    static constexpr int EFFECT_SINE      = 8;
    static constexpr int EFFECT_GRADIENT  = 9;
    static constexpr int EFFECT_SINELON   = 10;
    static constexpr int EFFECT_TWINKLE   = 11;
    static constexpr int EFFECT_SPARKLE   = 12;
    static constexpr int EFFECT_FIRE      = 13;
    static constexpr int EFFECT_CANDLE    = 14;
    static constexpr int EFFECT_METEOR    = 15;
    static constexpr int EFFECT_BOUNCING  = 16;
    static constexpr int EFFECT_WIPE      = 17;
    static constexpr int EFFECT_DUALSCAN  = 18;
    static constexpr int EFFECT_JUGGLE    = 19;
    static constexpr int EFFECT_MULTICOMET = 20;
    static constexpr int EFFECT_GLITTER   = 21;
    static constexpr int EFFECT_DISSOLVE  = 22;
    static constexpr int EFFECT_RIPPLE    = 23;
    static constexpr int EFFECT_DRIP      = 24;
    static constexpr int EFFECT_LIGHTNING = 25;
    static constexpr int EFFECT_FIREWORKS = 26;
    static constexpr int EFFECT_PLASMA    = 27;
    static constexpr int EFFECT_HEARTBEAT = 28;
    static constexpr int EFFECT_STROBE    = 29;
    static constexpr int EFFECT_POLICE    = 30;
    static constexpr int EFFECT_CHASE     = 31;
    static constexpr int EFFECT_RAILWAY   = 32;
    static constexpr int EFFECT_PACIFICA  = 33;
    static constexpr int EFFECT_AURORA    = 34;
    static constexpr int EFFECT_PRIDE     = 35;
    static constexpr int EFFECT_COLORWAVES = 36;
    static constexpr int EFFECT_BPM       = 37;
    static constexpr int EFFECT_BALL      = 38;  // LED tracks the sand ball's angle
    static constexpr int EFFECT_NONE      = 255;  // RunEffect/IdleEffect: no override

    static constexpr int DIR_CW  = 0;  // $LED/Direction values
    static constexpr int DIR_CCW = 1;

    static constexpr int kBalls = 3;

    void render();
    void renderEffect(int effect, uint8_t speed);  // fills _fb (0..255 per channel)
    void commit(uint8_t brightness);               // _fb -> _pixels, applies master brightness
    void parse_state_report();
    void setPixel(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
    void flushLive();  // persist any in-memory live overrides to NVS (called at idle)

    // Framebuffer helpers (operate on _fb, logical R,G,B order)
    void setFb(int i, uint8_t r, uint8_t g, uint8_t b);
    void addFb(int i, uint8_t r, uint8_t g, uint8_t b);
    void fadeBy(uint8_t amount);
    void getColor(StringSetting* s, uint8_t& r, uint8_t& g, uint8_t& b);
    void primaryColor(uint8_t& r, uint8_t& g, uint8_t& b);    // live _color override or setting
    void secondaryColor(uint8_t& r, uint8_t& g, uint8_t& b);  // live _color2 override or setting

    // Small fixed-point / math helpers (FastLED-style, self-contained)
    uint8_t        random8();
    uint8_t        random8(uint8_t lim);
    static uint8_t sin8(uint8_t theta);
    static uint8_t scale8(uint8_t v, uint8_t s);
    static uint8_t qadd8(uint8_t a, uint8_t b);
    static uint8_t qsub8(uint8_t a, uint8_t b);
    static uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t);
    static void    wheel(uint8_t pos, uint8_t& r, uint8_t& g, uint8_t& b);
    static void    heatColor(uint8_t heat, uint8_t& r, uint8_t& g, uint8_t& b);
    // Sample a 16-entry packed-RGB gradient table at 0..255 with interpolation
    static void    sampleTable(const uint32_t* pal, uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b);
    // Color from the active $LED/Palette (rainbow palette == wheel())
    void           palColor(uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b);

    // Configuration
    Pin         _data_pin;
    int         _num_leds    = 30;
    std::string _color_order = "GRB";
    int         _frame_ms    = 33;

    // Runtime settings ($LED/...)
    EnumSetting*   _effect      = nullptr;
    EnumSetting*   _palette     = nullptr;
    StringSetting* _color       = nullptr;
    StringSetting* _color2      = nullptr;
    IntSetting*    _brightness  = nullptr;
    IntSetting*    _speed       = nullptr;
    EnumSetting*   _run_effect  = nullptr;
    EnumSetting*   _idle_effect = nullptr;
    EnumSetting*   _direction   = nullptr;  // 'ball' effect: ring winding vs theta (cw/ccw)
    IntSetting*    _align       = nullptr;  // 'ball' effect: angular offset, degrees 0..359
    IntSetting*    _ballsize    = nullptr;  // 'ball' effect: glow radius in LEDs (size of the follow blob)

    // Machine-state tracking (poll task only: reports arrive via our
    // own autoReport, which runs inside pollLine)
    std::string _report;                 // partial report line being received
    int         _auto_effect = -1;       // -1: no override; else EFFECT_*

    // Live overrides ($Sand/Led / /sand_led).  Empty string = unset, so the
    // persisted setting is used.  Set while moving, flushed to NVS at idle.
    static Leds* _instance;
    std::string  _live_effect, _live_palette, _live_color, _live_color2, _live_bright, _live_speed;
    bool         _quiet_off = false;  // Still Sands: force strip off (in-memory, highest priority)
    std::string  _live_direction, _live_align, _live_ballsize;
    float        _ball_track = -1.0f;  // smoothed ball position [0,1) for the 'ball' effect
    bool         _was_running = false;
    int          _cur_palette = 0;       // palette id resolved once per frame

    // Driver state
    bool     _ready = false;
    uint8_t* _pixels = nullptr;  // 3 bytes per LED, in wire order
    uint8_t* _fb     = nullptr;  // 3 bytes per LED, logical R,G,B working buffer
    uint8_t* _heat   = nullptr;  // 1 byte per LED, fire effect heat
    int      _ri = 1, _gi = 0, _bi = 2;  // RGB -> wire-order byte offsets (GRB default)
    uint16_t _phase      = 0;  // animation phase, 8.8 fixed point
    uint32_t _last_frame = 0;

    // Per-effect persistent state (reset when the effect changes)
    int      _last_effect = -1;
    uint32_t _rng         = 0x1234abcdU;  // xorshift state
    uint8_t  _candle      = 200;          // candle flicker level
    uint8_t  _aux         = 0;            // scratch toggle (e.g. dissolve direction)
    float    _ball_pos[kBalls] = { 0, 0, 0 };  // also reused by ripple (center) / drip (height)
    float    _ball_vel[kBalls] = { 0, 0, 0 };  // also reused by ripple (radius) / drip (speed)
};
