// WLED effect compatibility layer: just enough of WLED 0.15's Segment API and
// FastLED's lib8tion / noise / color helpers that WLED's FX.cpp effects port
// nearly verbatim.  Std-only, so [env:tests] and tools/fx_parity run it on the
// host.  The app mirrors this file line for line in
// dune-weaver-mobile/src/lib/wledfx/shim.ts -- change one, change both.
//
// Ported from WLED v0.15.3 (Copyright (c) 2016-present Christian Schwinne and
// individual WLED contributors, EUPL-1.2) and FastLED 3.6.0 (MIT).  The EUPL
// code is redistributed under GPLv3 per EUPL-1.2 Article 5 (GPLv3 is a listed
// compatible licence).
//
// Effects include this header and are written as `uint16_t mode_x(void)`
// against the SEGMENT / SEGENV / SEGLEN / SEGCOLOR / strip macros below.  Only
// effect files include it: the macros would collide with anything else.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <initializer_list>

// No fused multiply-add in the layer or any effect that includes it: GCC (the
// ESP32's madd.s) and clang (arm64 fmadd) fuse `a * b + c` by default, which
// rounds once where the app's JS rounds twice -- the frames would drift.
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace wled {

typedef uint8_t  byte;
typedef uint8_t  fract8;
typedef uint16_t fract16;
typedef uint16_t accum88;
typedef int16_t  saccum87;

constexpr uint16_t FRAMETIME_MS = 33;  // Leds frame_ms default; WLED's is 1000/42
constexpr int      NUM_COLORS   = 3;
constexpr uint8_t  DEFAULT_C1 = 128, DEFAULT_C2 = 128, DEFAULT_C3 = 16;

// ---- lib8tion (FastLED 3.6.0, C paths, FASTLED_SCALE8_FIXED / BLEND_FIXED = 1)
inline uint8_t  scale8(uint8_t i, fract8 scale) { return (((uint16_t)i) * (1 + (uint16_t)scale)) >> 8; }
inline uint8_t  scale8_video(uint8_t i, fract8 scale) { return (((int)i * (int)scale) >> 8) + ((i && scale) ? 1 : 0); }
inline uint16_t scale16by8(uint16_t i, fract8 scale) { return (i * (1 + ((uint16_t)scale))) >> 8; }
inline uint16_t scale16(uint16_t i, fract16 scale) { return ((uint32_t)i * (1 + (uint32_t)scale)) / 65536; }
inline void     nscale8x3(uint8_t& r, uint8_t& g, uint8_t& b, fract8 scale) {
    uint16_t s = scale + 1;
    r = ((uint16_t)r * s) >> 8;
    g = ((uint16_t)g * s) >> 8;
    b = ((uint16_t)b * s) >> 8;
}
inline void nscale8x3_video(uint8_t& r, uint8_t& g, uint8_t& b, fract8 scale) {
    uint8_t nz = (scale != 0) ? 1 : 0;
    r = (r == 0) ? 0 : (((int)r * (int)scale) >> 8) + nz;
    g = (g == 0) ? 0 : (((int)g * (int)scale) >> 8) + nz;
    b = (b == 0) ? 0 : (((int)b * (int)scale) >> 8) + nz;
}
inline uint8_t qadd8(uint8_t i, uint8_t j) { unsigned t = i + j; return t > 255 ? 255 : t; }
inline uint8_t qsub8(uint8_t i, uint8_t j) { int t = i - j; return t < 0 ? 0 : t; }
inline uint8_t qmul8(uint8_t i, uint8_t j) { unsigned p = (unsigned)i * j; return p > 255 ? 255 : p; }
inline uint8_t add8(uint8_t i, uint8_t j) { return i + j; }
inline uint8_t sub8(uint8_t i, uint8_t j) { return i - j; }
inline uint8_t mul8(uint8_t i, uint8_t j) { return ((int)i * (int)j) & 0xFF; }
inline uint8_t avg8(uint8_t i, uint8_t j) { return (i + j) >> 1; }
inline int8_t  avg7(int8_t i, int8_t j) { return (i >> 1) + (j >> 1) + (i & 0x1); }
inline int16_t avg15(int16_t i, int16_t j) { return (i >> 1) + (j >> 1) + (i & 0x1); }
inline uint8_t abs8(int8_t i) { return i < 0 ? -i : i; }
inline uint8_t blend8(uint8_t a, uint8_t b, uint8_t amountOfB) {
    uint16_t partial = (a << 8) | b;
    partial += (b * amountOfB);
    partial -= (a * amountOfB);
    return partial >> 8;
}
inline uint8_t lerp8by8(uint8_t a, uint8_t b, fract8 frac) {
    return b > a ? (uint8_t)(a + scale8(b - a, frac)) : (uint8_t)(a - scale8(a - b, frac));
}
inline uint16_t lerp16by16(uint16_t a, uint16_t b, fract16 frac) {
    return b > a ? (uint16_t)(a + scale16(b - a, frac)) : (uint16_t)(a - scale16(a - b, frac));
}
inline int16_t lerp15by16(int16_t a, int16_t b, fract16 frac) {
    return b > a ? (int16_t)(a + scale16((uint16_t)(b - a), frac)) : (int16_t)(a - scale16((uint16_t)(a - b), frac));
}
inline uint8_t map8(uint8_t in, uint8_t rangeStart, uint8_t rangeEnd) { return scale8(in, rangeEnd - rangeStart) + rangeStart; }
uint8_t        sqrt16(uint16_t x);
inline uint8_t ease8InOutQuad(uint8_t i) {
    uint8_t j = (i & 0x80) ? 255 - i : i;
    uint8_t jj2 = scale8(j, j) << 1;
    return (i & 0x80) ? 255 - jj2 : jj2;
}
inline uint16_t ease16InOutQuad(uint16_t i) {
    uint16_t j = (i & 0x8000) ? 65535 - i : i;
    uint16_t jj2 = scale16(j, j) << 1;
    return (i & 0x8000) ? 65535 - jj2 : jj2;
}
inline uint8_t ease8InOutCubic(uint8_t i) {
    uint8_t  ii  = scale8(i, i);
    uint8_t  iii = scale8(ii, i);
    uint16_t r1  = (3 * (uint16_t)ii) - (2 * (uint16_t)iii);
    return (r1 & 0x100) ? 255 : (uint8_t)r1;
}
inline uint8_t ease8InOutApprox(uint8_t i) {
    if (i < 64) {
        i /= 2;
    } else if (i > (255 - 64)) {
        i = 255 - i;
        i /= 2;
        i = 255 - i;
    } else {
        i -= 64;
        i += (i / 2);
        i += 32;
    }
    return i;
}
inline uint8_t  triwave8(uint8_t in) { if (in & 0x80) in = 255 - in; return in << 1; }
inline uint16_t triwave16(uint16_t in) { if (in < 0x8000) return in * 2; return 0xFFFF - (in - 0x8000) * 2; }
inline uint8_t  quadwave8(uint8_t in) { return ease8InOutQuad(triwave8(in)); }
inline uint8_t  cubicwave8(uint8_t in) { return ease8InOutCubic(triwave8(in)); }
inline uint8_t  squarewave8(uint8_t in, uint8_t pulsewidth = 128) { return in < pulsewidth || pulsewidth == 255 ? 255 : 0; }

// RNG: FastLED's 16-bit LCG.  One generator for every effect (as in WLED);
// the parity harness seeds it, the firmware seeds it from the clock.
extern uint16_t rand16seed;
inline uint8_t random8() {
    rand16seed = (rand16seed * 2053) + 13849;
    return (uint8_t)(((uint8_t)(rand16seed & 0xFF)) + ((uint8_t)(rand16seed >> 8)));
}
inline uint8_t  random8(uint8_t lim) { return (random8() * lim) >> 8; }
inline uint8_t  random8(uint8_t min, uint8_t lim) { return random8(lim - min) + min; }
inline uint16_t random16() { rand16seed = (rand16seed * 2053) + 13849; return rand16seed; }
inline uint16_t random16(uint16_t lim) { return ((uint32_t)random16() * lim) >> 16; }
inline uint16_t random16(uint16_t min, uint16_t lim) { return random16(lim - min) + min; }
inline void     random16_set_seed(uint16_t seed) { rand16seed = seed; }
inline uint16_t random16_get_seed() { return rand16seed; }
inline void     random16_add_entropy(uint16_t entropy) { rand16seed += entropy; }
// Arduino random(): only its range matters to the effects
inline long random(long howbig) {
    if (howbig <= 0) return 0;
    uint32_t hi = random16();  // sequenced: `a << 16 | b` leaves the order open
    uint32_t lo = random16();
    return (long)(((hi << 16) | lo) % (uint32_t)howbig);
}
inline long random(long howsmall, long howbig) { return howsmall >= howbig ? howsmall : howsmall + random(howbig - howsmall); }

// Arduino helpers
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    if (in_max == in_min) return out_min;
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
inline float mapf(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define qsuba(x, b) ((x > b) ? x - b : 0)
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
using std::min;
using std::max;
using std::abs;

// ---- WLED integer trig (wled_math.cpp)
int16_t sin16_t(uint16_t theta);
inline int16_t cos16_t(uint16_t theta) { return sin16_t(theta + 0x4000); }
uint8_t        sin8_t(uint8_t theta);
inline uint8_t cos8_t(uint8_t theta) { return sin8_t(theta + 64); }
float          sin_approx(float theta);
float          cos_approx(float theta);
float          tan_approx(float x);
float          atan2_t(float y, float x);
float          acos_t(float x);
float          asin_t(float x);
float          atan_t(float x);
float          floor_t(float x);
float          fmod_t(float num, float denom);
#define sin_t sin_approx
#define cos_t cos_approx
#define tan_t tan_approx

// ---- beats (time = the effect clock, strip.now)
extern uint32_t now_ms;
inline uint16_t beat88(accum88 bpm88, uint32_t timebase = 0) { return (((now_ms)-timebase) * bpm88 * 280) >> 16; }
inline uint16_t beat16(accum88 bpm, uint32_t timebase = 0) { if (bpm < 256) bpm <<= 8; return beat88(bpm, timebase); }
inline uint8_t  beat8(accum88 bpm, uint32_t timebase = 0) { return beat16(bpm, timebase) >> 8; }
uint16_t        beatsin88_t(accum88 bpm88, uint16_t lowest = 0, uint16_t highest = 65535, uint32_t timebase = 0, uint16_t phase_offset = 0);
uint16_t        beatsin16_t(accum88 bpm, uint16_t lowest = 0, uint16_t highest = 65535, uint32_t timebase = 0, uint16_t phase_offset = 0);
uint8_t         beatsin8_t(accum88 bpm, uint8_t lowest = 0, uint8_t highest = 255, uint32_t timebase = 0, uint8_t phase_offset = 0);
inline uint32_t millis() { return now_ms; }
inline uint32_t micros() { return now_ms * 1000; }

// ---- noise (FastLED noise.cpp, FASTLED_NOISE_FIXED = 1)
int16_t  inoise16_raw(uint32_t x, uint32_t y, uint32_t z);
int16_t  inoise16_raw(uint32_t x, uint32_t y);
int16_t  inoise16_raw(uint32_t x);
uint16_t inoise16(uint32_t x, uint32_t y, uint32_t z);
uint16_t inoise16(uint32_t x, uint32_t y);
uint16_t inoise16(uint32_t x);
int8_t   inoise8_raw(uint16_t x, uint16_t y, uint16_t z);
int8_t   inoise8_raw(uint16_t x, uint16_t y);
int8_t   inoise8_raw(uint16_t x);
uint8_t  inoise8(uint16_t x, uint16_t y, uint16_t z);
uint8_t  inoise8(uint16_t x, uint16_t y);
uint8_t  inoise8(uint16_t x);

// ---- colors
#define RGBW32(r, g, b, w) (uint32_t((::wled::byte(w) << 24) | (::wled::byte(r) << 16) | (::wled::byte(g) << 8) | (::wled::byte(b))))
#define R(c) (::wled::byte((c) >> 16))
#define G(c) (::wled::byte((c) >> 8))
#define B(c) (::wled::byte(c))
#define W(c) (::wled::byte((c) >> 24))
constexpr uint32_t RED = 0xFF0000, GREEN = 0x00FF00, BLUE = 0x0000FF, WHITE = 0xFFFFFF, BLACK = 0x000000;
constexpr uint32_t YELLOW = 0xFFFF00, CYAN = 0x00FFFF, MAGENTA = 0xFF00FF, PURPLE = 0x400080, ORANGE = 0xFF3000;
constexpr uint32_t PINK = 0xFF1493, GREY = 0x808080, GRAY = 0x808080, DARKGREY = 0x333333, DARKGRAY = 0x333333;
constexpr uint32_t ULTRAWHITE = 0xFFFFFFFF, DARKSLATEGRAY = 0x2F4F4F, DARKSLATEGREY = 0x2F4F4F;

struct CHSV;
struct CRGB {
    union {
        struct { uint8_t r, g, b; };
        struct { uint8_t red, green, blue; };
        uint8_t raw[3];
    };
    // The FastLED HTML colors the effects use (extend as needed: values from pixeltypes.h)
    enum HTMLColorCode : uint32_t {
        Black = 0x000000, White = 0xFFFFFF, Red = 0xFF0000, Green = 0x008000, Blue = 0x0000FF, Yellow = 0xFFFF00,
        Orange = 0xFFA500, DarkOrange = 0xFF8C00, OrangeRed = 0xFF4500, Gray = 0x808080, Grey = 0x808080,
        DarkGray = 0xA9A9A9, Purple = 0x800080, Magenta = 0xFF00FF, Cyan = 0x00FFFF, Aqua = 0x00FFFF, Pink = 0xFFC0CB,
        HotPink = 0xFF69B4, DeepPink = 0xFF1493, Gold = 0xFFD700, Navy = 0x000080, DarkBlue = 0x00008B, Teal = 0x008080,
        Lime = 0x00FF00, LimeGreen = 0x32CD32, DarkGreen = 0x006400, Brown = 0xA52A2A, Maroon = 0x800000,
        DarkRed = 0x8B0000, Violet = 0xEE82EE, Indigo = 0x4B0082, SkyBlue = 0x87CEEB, LightBlue = 0xADD8E6,
        DeepSkyBlue = 0x00BFFF, Amethyst = 0x9966CC, FairyLight = 0xFFE42D, Salmon = 0xFA8072, Crimson = 0xDC143C,
        Coral = 0xFF7F50, Tomato = 0xFF6347, Turquoise = 0x40E0D0, Seashell = 0xFFF5EE, Snow = 0xFFFAFA,
        SeaGreen = 0x2E8B57, Chartreuse = 0x7FFF00, DarkViolet = 0x9400D3, MidnightBlue = 0x191970,
    };
    CRGB() : r(0), g(0), b(0) {}
    CRGB(uint8_t ir, uint8_t ig, uint8_t ib) : r(ir), g(ig), b(ib) {}
    CRGB(uint32_t colorcode) : r((colorcode >> 16) & 0xFF), g((colorcode >> 8) & 0xFF), b(colorcode & 0xFF) {}
    CRGB(HTMLColorCode colorcode) : CRGB((uint32_t)colorcode) {}
    CRGB(const CHSV& hsv);
    CRGB& operator=(uint32_t colorcode) { r = (colorcode >> 16) & 0xFF; g = (colorcode >> 8) & 0xFF; b = colorcode & 0xFF; return *this; }
    uint8_t&       operator[](uint8_t x) { return raw[x]; }
    const uint8_t& operator[](uint8_t x) const { return raw[x]; }
    CRGB&          setRGB(uint8_t nr, uint8_t ng, uint8_t nb) { r = nr; g = ng; b = nb; return *this; }
    CRGB&          setHSV(uint8_t hue, uint8_t sat, uint8_t val);
    CRGB&          setHue(uint8_t hue);
    CRGB& operator+=(const CRGB& rhs) { r = qadd8(r, rhs.r); g = qadd8(g, rhs.g); b = qadd8(b, rhs.b); return *this; }
    CRGB& operator-=(const CRGB& rhs) { r = qsub8(r, rhs.r); g = qsub8(g, rhs.g); b = qsub8(b, rhs.b); return *this; }
    CRGB& operator/=(uint8_t d) { r /= d; g /= d; b /= d; return *this; }
    CRGB& operator>>=(uint8_t d) { r >>= d; g >>= d; b >>= d; return *this; }
    CRGB& operator*=(uint8_t d) { r = qmul8(r, d); g = qmul8(g, d); b = qmul8(b, d); return *this; }
    CRGB& nscale8_video(uint8_t s) { nscale8x3_video(r, g, b, s); return *this; }
    CRGB& operator%=(uint8_t s) { nscale8x3_video(r, g, b, s); return *this; }
    CRGB& fadeLightBy(uint8_t f) { nscale8x3_video(r, g, b, 255 - f); return *this; }
    CRGB& nscale8(uint8_t s) { nscale8x3(r, g, b, s); return *this; }
    CRGB& nscale8(const CRGB& s) { r = scale8(r, s.r); g = scale8(g, s.g); b = scale8(b, s.b); return *this; }
    CRGB& fadeToBlackBy(uint8_t f) { nscale8x3(r, g, b, 255 - f); return *this; }
    CRGB& operator|=(const CRGB& rhs) { if (rhs.r > r) r = rhs.r; if (rhs.g > g) g = rhs.g; if (rhs.b > b) b = rhs.b; return *this; }
    CRGB& operator|=(uint8_t d) { if (d > r) r = d; if (d > g) g = d; if (d > b) b = d; return *this; }
    CRGB& operator&=(const CRGB& rhs) { if (rhs.r < r) r = rhs.r; if (rhs.g < g) g = rhs.g; if (rhs.b < b) b = rhs.b; return *this; }
    explicit operator bool() const { return r || g || b; }
    // WLED treats a CRGB as RGBW32(r,g,b,0); FastLED's own cast would set W=255.
    explicit operator uint32_t() const { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
    CRGB    operator-() const { return CRGB(255 - r, 255 - g, 255 - b); }
    uint8_t getLuma() const { return scale8(r, 54) + scale8(g, 183) + scale8(b, 18); }
    uint8_t getAverageLight() const { return scale8(r, 85) + scale8(g, 85) + scale8(b, 85); }
    void    maximizeBrightness(uint8_t limit = 255) {
        uint8_t m = r;
        if (g > m) m = g;
        if (b > m) m = b;
        if (m > 0) {
            uint16_t factor = ((uint16_t)(limit) * 256) / m;
            r = (r * factor) / 256;
            g = (g * factor) / 256;
            b = (b * factor) / 256;
        }
    }
};
inline bool operator==(const CRGB& a, const CRGB& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(const CRGB& a, const CRGB& b) { return !(a == b); }
inline CRGB operator+(const CRGB& a, const CRGB& b) { return CRGB(qadd8(a.r, b.r), qadd8(a.g, b.g), qadd8(a.b, b.b)); }
inline CRGB operator-(const CRGB& a, const CRGB& b) { return CRGB(qsub8(a.r, b.r), qsub8(a.g, b.g), qsub8(a.b, b.b)); }
inline CRGB operator*(const CRGB& p1, uint8_t d) { return CRGB(qmul8(p1.r, d), qmul8(p1.g, d), qmul8(p1.b, d)); }
inline CRGB operator%(const CRGB& p1, uint8_t d) { CRGB r = p1; r.nscale8_video(d); return r; }
inline CRGB operator|(const CRGB& a, const CRGB& b) { CRGB r = a; r |= b; return r; }

struct CHSV {
    union {
        struct { uint8_t hue, sat, val; };
        struct { uint8_t h, s, v; };
        uint8_t raw[3];
    };
    CHSV() : hue(0), sat(0), val(0) {}
    CHSV(uint8_t ih, uint8_t is, uint8_t iv) : hue(ih), sat(is), val(iv) {}
};
void        hsv2rgb_rainbow(const CHSV& hsv, CRGB& rgb);
inline CRGB::CRGB(const CHSV& hsv) { hsv2rgb_rainbow(hsv, *this); }
inline CRGB& CRGB::setHSV(uint8_t hue, uint8_t sat, uint8_t val) { hsv2rgb_rainbow(CHSV(hue, sat, val), *this); return *this; }
inline CRGB& CRGB::setHue(uint8_t hue) { hsv2rgb_rainbow(CHSV(hue, 255, 255), *this); return *this; }

enum TBlendType { NOBLEND = 0, LINEARBLEND = 1, LINEARBLEND_NOWRAP = 2 };
void fill_solid(CRGB* leds, int numToFill, const CRGB& color);
void fill_gradient_RGB(CRGB* leds, uint16_t startpos, CRGB startcolor, uint16_t endpos, CRGB endcolor);
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2);
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2, const CRGB& c3);
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2, const CRGB& c3, const CRGB& c4);

struct CRGBPalette16 {
    CRGB entries[16];
    CRGBPalette16() {}
    explicit CRGBPalette16(const uint32_t* packed16) { for (int i = 0; i < 16; i++) entries[i] = CRGB(packed16[i]); }
    // CRGBPalette16 p = { 0x000000, ... 16 colors }, as WLED's effects write their own
    CRGBPalette16(std::initializer_list<uint32_t> l) { int i = 0; for (uint32_t c : l) if (i < 16) entries[i++] = CRGB(c); }
    CRGBPalette16(const CRGB& c1) { fill_solid(entries, 16, c1); }
    CRGBPalette16(const CRGB& c1, const CRGB& c2) { fill_gradient_RGB(entries, 16, c1, c2); }
    CRGBPalette16(const CRGB& c1, const CRGB& c2, const CRGB& c3) { fill_gradient_RGB(entries, 16, c1, c2, c3); }
    CRGBPalette16(const CRGB& c1, const CRGB& c2, const CRGB& c3, const CRGB& c4) { fill_gradient_RGB(entries, 16, c1, c2, c3, c4); }
    CRGB&       operator[](uint8_t x) { return entries[x]; }
    const CRGB& operator[](uint8_t x) const { return entries[x]; }
};
CRGB     ColorFromPalette(const CRGBPalette16& pal, uint8_t index, uint8_t brightness = 255, TBlendType blendType = LINEARBLEND);
CRGB     HeatColor(uint8_t temperature);
CRGB&    nblend(CRGB& existing, const CRGB& overlay, fract8 amountOfOverlay);
CRGB     blend(const CRGB& p1, const CRGB& p2, fract8 amountOfP2);
void     nblendPaletteTowardPalette(CRGBPalette16& current, CRGBPalette16& target, uint8_t maxChanges);
uint32_t color_blend(uint32_t c1, uint32_t c2, uint16_t blend, bool b16 = false);
uint32_t color_add(uint32_t c1, uint32_t c2, bool fast = false);
uint32_t color_fade(uint32_t c1, uint8_t amount, bool video = false);
uint8_t  get_random_wheel_index(uint8_t pos);
uint8_t  gamma8(uint8_t b);  // WLED's 2.8 gamma table
uint32_t gamma32(uint32_t c);

// ---- the segment (one per strip: no segments, mirroring or grouping here)
struct Segment {
    // What the user set ($LED/*)
    uint8_t  speed = 128, intensity = 128;
    uint8_t  custom1 = DEFAULT_C1, custom2 = DEFAULT_C2, custom3 = DEFAULT_C3;  // WLED's extra sliders: effect defaults
    bool     check1 = false, check2 = false, check3 = false;                     // WLED's checkboxes: effect defaults
    uint32_t colors[NUM_COLORS] = { 0, 0, 0 };
    uint8_t  palette = 0;  // $LED/Palette id; 0 = WLED's "Default"
    // Effect state, reset when the effect changes (WLED's SEGENV)
    uint32_t call = 0;
    uint32_t step = 0;
    uint16_t aux0 = 0, aux1 = 0;
    uint8_t* data     = nullptr;
    size_t   _dataLen = 0;
    uint32_t next_time = 0;
    // Geometry: a 1D strip of _w pixels, or a _w x _h matrix (2D effects)
    uint16_t  _w = 1, _h = 1;
    uint32_t* _px = nullptr;  // _w * _h pixels, owned by the Runner
    CRGBPalette16 _currentPalette;

    bool     is2D() const { return _h > 1; }
    uint16_t virtualWidth() const { return _w; }
    uint16_t virtualHeight() const { return _h; }
    uint16_t virtualLength() const { return _w * _h; }
    uint16_t length() const { return _w * _h; }
    uint16_t width() const { return _w; }
    uint16_t height() const { return _h; }
    uint8_t  nrOfVStrips() const { return 1; }
    bool     isActive() const { return true; }
    uint32_t currentColor(uint8_t slot) const { return slot < NUM_COLORS ? colors[slot] : 0; }

    bool allocateData(size_t len);
    void deallocateData();

    void     setPixelColor(int i, uint32_t c);
    void     setPixelColor(unsigned n, uint32_t c) { setPixelColor(int(n), c); }
    void     setPixelColor(int n, byte r, byte g, byte b, byte w = 0) { setPixelColor(n, RGBW32(r, g, b, w)); }
    void     setPixelColor(int n, CRGB c) { setPixelColor(n, RGBW32(c.r, c.g, c.b, 0)); }
    void     setPixelColor(float i, uint32_t c, bool aa = true);
    void     setPixelColor(float i, uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0, bool aa = true) { setPixelColor(i, RGBW32(r, g, b, w), aa); }
    void     setPixelColor(float i, CRGB c, bool aa = true) { setPixelColor(i, RGBW32(c.r, c.g, c.b, 0), aa); }
    uint32_t getPixelColor(int i) const;
    void     blendPixelColor(int n, uint32_t color, uint8_t blend) { setPixelColor(n, color_blend(getPixelColor(n), color, blend)); }
    void     blendPixelColor(int n, CRGB c, uint8_t blend) { blendPixelColor(n, RGBW32(c.r, c.g, c.b, 0), blend); }
    void     addPixelColor(int n, uint32_t color, bool fast = false) { setPixelColor(n, color_add(getPixelColor(n), color, fast)); }
    void     addPixelColor(int n, byte r, byte g, byte b, byte w = 0, bool fast = false) { addPixelColor(n, RGBW32(r, g, b, w), fast); }
    void     addPixelColor(int n, CRGB c, bool fast = false) { addPixelColor(n, RGBW32(c.r, c.g, c.b, 0), fast); }
    void     fadePixelColor(uint16_t n, uint8_t fade) { setPixelColor(n, color_fade(getPixelColor(n), fade, true)); }

    void     fill(uint32_t c);
    void     fade_out(uint8_t rate);
    void     fadeToBlackBy(uint8_t fadeBy);
    void     blur(uint8_t blur_amount, bool smear = false);
    uint32_t color_wheel(uint8_t pos) const;
    uint32_t color_from_palette(uint16_t i, bool mapping, bool wrap, uint8_t mcol, uint8_t pbri = 255) const;
    uint8_t  gamma8(uint8_t b) const { return ::wled::gamma8(b); }

    // 2D (FX_2Dfcn.cpp)
    uint16_t XY(int x, int y) const { return (x % _w) + (y % _h) * _w; }
    void     setPixelColorXY(int x, int y, uint32_t c);
    void     setPixelColorXY(unsigned x, unsigned y, uint32_t c) { setPixelColorXY(int(x), int(y), c); }
    void     setPixelColorXY(int x, int y, byte r, byte g, byte b, byte w = 0) { setPixelColorXY(x, y, RGBW32(r, g, b, w)); }
    void     setPixelColorXY(int x, int y, CRGB c) { setPixelColorXY(x, y, RGBW32(c.r, c.g, c.b, 0)); }
    void     setPixelColorXY(unsigned x, unsigned y, CRGB c) { setPixelColorXY(int(x), int(y), RGBW32(c.r, c.g, c.b, 0)); }
    void     setPixelColorXY(float x, float y, uint32_t c, bool aa = true);
    void     setPixelColorXY(float x, float y, byte r, byte g, byte b, byte w = 0, bool aa = true) { setPixelColorXY(x, y, RGBW32(r, g, b, w), aa); }
    void     setPixelColorXY(float x, float y, CRGB c, bool aa = true) { setPixelColorXY(x, y, RGBW32(c.r, c.g, c.b, 0), aa); }
    uint32_t getPixelColorXY(int x, int y) const;
    void     blendPixelColorXY(uint16_t x, uint16_t y, uint32_t color, uint8_t blend) { setPixelColorXY(x, y, color_blend(getPixelColorXY(x, y), color, blend)); }
    void     blendPixelColorXY(uint16_t x, uint16_t y, CRGB c, uint8_t blend) { blendPixelColorXY(x, y, RGBW32(c.r, c.g, c.b, 0), blend); }
    void     addPixelColorXY(int x, int y, uint32_t color, bool fast = false) { setPixelColorXY(x, y, color_add(getPixelColorXY(x, y), color, fast)); }
    void     addPixelColorXY(int x, int y, byte r, byte g, byte b, byte w = 0, bool fast = false) { addPixelColorXY(x, y, RGBW32(r, g, b, w), fast); }
    void     addPixelColorXY(int x, int y, CRGB c, bool fast = false) { addPixelColorXY(x, y, RGBW32(c.r, c.g, c.b, 0), fast); }
    void     fadePixelColorXY(uint16_t x, uint16_t y, uint8_t fade) { setPixelColorXY(x, y, color_fade(getPixelColorXY(x, y), fade, true)); }
    void     blurRow(uint32_t row, fract8 blur_amount, bool smear = false);
    void     blurCol(uint32_t col, fract8 blur_amount, bool smear = false);
    void     blurRows(fract8 blur_amount, bool smear = false) { for (unsigned r = 0; r < _h; r++) blurRow(r, blur_amount, smear); }
    void     blurCols(fract8 blur_amount, bool smear = false) { for (unsigned c = 0; c < _w; c++) blurCol(c, blur_amount, smear); }
    void     blur2D(uint8_t blur_amount, bool smear = false);
    void     moveX(int8_t delta, bool wrap = false);
    void     moveY(int8_t delta, bool wrap = false);
    void     move(uint8_t dir, uint8_t delta, bool wrap = false);
    void     drawCircle(uint16_t cx, uint16_t cy, uint8_t radius, uint32_t c, bool soft = false);
    void     drawCircle(uint16_t cx, uint16_t cy, uint8_t radius, CRGB c, bool soft = false) { drawCircle(cx, cy, radius, RGBW32(c.r, c.g, c.b, 0), soft); }
    void     fillCircle(uint16_t cx, uint16_t cy, uint8_t radius, uint32_t c, bool soft = false);
    void     fillCircle(uint16_t cx, uint16_t cy, uint8_t radius, CRGB c, bool soft = false) { fillCircle(cx, cy, radius, RGBW32(c.r, c.g, c.b, 0), soft); }
    void     drawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint32_t c, bool soft = false);
    void     drawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, CRGB c, bool soft = false) { drawLine(x0, y0, x1, y1, RGBW32(c.r, c.g, c.b, 0), soft); }
    void     wu_pixel(uint32_t x, uint32_t y, CRGB c);
    void     nscale8(uint8_t scale) { for (int i = 0; i < _w * _h; i++) _px[i] = color_fade(_px[i], scale); }
};

// strip: the WLED globals effects read
struct Strip {
    uint32_t now          = 0;
    bool     isMatrix     = false;
    uint8_t  paletteBlend = 0;  // WLED default: wrap when moving
    uint8_t  getBrightness() const { return 255; }
    uint8_t  getActiveSegmentsNum() const { return 1; }
    uint8_t  getCurrSegmentId() const { return 0; }
    uint8_t  getMaxSegments() const { return 1; }
    uint16_t getFrameTime() const { return FRAMETIME_MS; }
};
extern Strip    strip;
extern Segment* _seg;

uint16_t mode_static(void);
typedef uint16_t (*mode_ptr)(void);

}  // namespace wled

#define SEGMENT (*::wled::_seg)
#define SEGENV (*::wled::_seg)
#define SEGLEN (::wled::_seg->virtualLength())
#define SEG_W (::wled::_seg->virtualWidth())
#define SEG_H (::wled::_seg->virtualHeight())
#define SEGCOLOR(x) (::wled::_seg->currentColor(x))
#define SEGPALETTE (::wled::_seg->_currentPalette)
#define FRAMETIME (::wled::FRAMETIME_MS)
#define PALETTE_SOLID_WRAP (::wled::strip.paletteBlend == 1 || ::wled::strip.paletteBlend == 3)
#define PALETTE_MOVING_WRAP !(::wled::strip.paletteBlend == 2 || (::wled::strip.paletteBlend == 0 && SEGMENT.speed == 0))
#define indexToVStrip(index, stripNr) ((index) | (int((stripNr) + 1) << 16))
#define PSTR(s) (s)
