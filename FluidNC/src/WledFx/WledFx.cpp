// WLED / FastLED compatibility layer (see WledFx.h).  Mirrored line for line by
// dune-weaver-mobile/src/lib/wledfx/shim.ts.
// Ported from WLED v0.15.3 (EUPL-1.2, redistributed under GPLv3 per its
// Article 5) and FastLED 3.6.0 (MIT).
#include "WledFx.h"

namespace wled {

uint16_t rand16seed = 1337;
uint32_t now_ms     = 0;
Strip    strip;
Segment* _seg = nullptr;

uint16_t mode_static(void) {
    SEGMENT.fill(SEGCOLOR(0));
    return 350;
}

// ---- lib8tion -------------------------------------------------------------

uint8_t sqrt16(uint16_t x) {
    if (x <= 1) {
        return x;
    }
    uint8_t low = 1;
    uint8_t hi, mid;
    if (x > 7904) {
        hi = 255;
    } else {
        hi = (x >> 5) + 8;
    }
    do {
        mid = (low + hi) >> 1;
        if ((uint16_t)(mid * mid) > x) {
            hi = mid - 1;
        } else {
            if (mid == 255) {
                return 255;
            }
            low = mid + 1;
        }
    } while (hi >= low);
    return low - 1;
}

// ---- wled_math.cpp ----------------------------------------------------------

int16_t sin16_t(uint16_t theta) {
    int scale = 1;
    if (theta > 0x7FFF) {
        theta = 0xFFFF - theta;
        scale = -1;
    }
    uint32_t precal      = theta * (0x7FFF - theta);
    uint64_t numerator   = (uint64_t)precal * (4 * 0x7FFF);
    int32_t  denominator = 1342095361 - precal;
    int16_t  result      = numerator / denominator;
    return result * scale;
}

uint8_t sin8_t(uint8_t theta) {
    int32_t sin16 = sin16_t((uint16_t)theta * 257);
    sin16 += 0x7FFF + 128;
    return std::min(sin16, int32_t(0xFFFF)) >> 8;
}

static constexpr float M_TWOPI_F = 6.283185307179586f;
static constexpr float M_PI_F    = 3.141592653589793f;
static constexpr float M_PI_2_F  = 1.5707963267948966f;
static constexpr float M_PI_4_F  = 0.7853981633974483f;

float sin_approx(float theta) {
    uint16_t scaled_theta = (int)(theta * (float)(0xFFFF / M_TWOPI_F));
    int32_t  result       = sin16_t(scaled_theta);
    return float(result) / 0x7FFF;
}
float cos_approx(float theta) {
    uint16_t scaled_theta = (int)(theta * (float)(0xFFFF / M_TWOPI_F));
    int32_t  result       = sin16_t(scaled_theta + 0x4000);
    return float(result) / 0x7FFF;
}
float tan_approx(float x) {
    float c = cos_approx(x);
    if (c == 0.0f) {
        return 0;
    }
    return sin_approx(x) / c;
}
float atan2_t(float y, float x) {
    float abs_y = std::fabs(y);
    float abs_x = std::fabs(x);
    float r     = (abs_x - abs_y) / (abs_y + abs_x + 1e-10f);
    float angle;
    if (x < 0) {
        r     = -r;
        angle = M_PI_2_F + M_PI_4_F;
    } else {
        angle = M_PI_2_F - M_PI_4_F;
    }
    float add = (0.1963f * (r * r) - 0.9817f) * r;
    angle += add;
    return y < 0 ? -angle : angle;
}
float acos_t(float x) {
    float negate = float(x < 0);
    float xabs   = std::fabs(x);
    float ret    = -0.0187293f;
    ret          = ret * xabs;
    ret          = ret + 0.0742610f;
    ret          = ret * xabs;
    ret          = ret - 0.2121144f;
    ret          = ret * xabs;
    ret          = ret + M_PI_2_F;
    ret          = ret * std::sqrt(1.0f - xabs);
    ret          = ret - 2 * negate * ret;
    return negate * M_PI_F + ret;
}
float asin_t(float x) { return M_PI_2_F - acos_t(x); }
float atan_t(float x) {
    // WLED computes the <= 1 branch in double: kept, so the app matches.
    static const double A  = 0.0776509570923569;
    static const double Bc = -0.287434475393028;
    static const double C  = ((0.7853981633974483) - A - Bc);
    bool                neg = (x < 0);
    x                       = std::fabs(x);
    float res;
    if (x > 5.0f) {
        res = M_PI_2_F - (1.0f / x);
    } else if (x > 1.0f) {
        float xx = x * x;
        res      = (-0.003445f * xx * xx) + (0.05375f * xx * x) + (-0.326175f * xx) + (0.974207f * x) + 0.089494f;
    } else {
        float xx = x * x;
        res      = ((A * xx + Bc) * xx + C) * x;
    }
    return neg ? -res : res;
}
float floor_t(float x) {
    bool neg = x < 0;
    int  val = x;
    if (neg) {
        val--;
    }
    return val;
}
float fmod_t(float num, float denom) {
    int tquot = num / denom;
    return num - tquot * denom;
}

// ---- util.cpp beats -----------------------------------------------------------

uint16_t beatsin88_t(accum88 bpm88, uint16_t lowest, uint16_t highest, uint32_t timebase, uint16_t phase_offset) {
    uint16_t beat    = beat88(bpm88, timebase);
    uint16_t beatsin = (sin16_t(beat + phase_offset) + 32768);
    return lowest + scale16(beatsin, highest - lowest);
}
uint16_t beatsin16_t(accum88 bpm, uint16_t lowest, uint16_t highest, uint32_t timebase, uint16_t phase_offset) {
    uint16_t beat    = beat16(bpm, timebase);
    uint16_t beatsin = (sin16_t(beat + phase_offset) + 32768);
    return lowest + scale16(beatsin, highest - lowest);
}
uint8_t beatsin8_t(accum88 bpm, uint8_t lowest, uint8_t highest, uint32_t timebase, uint8_t phase_offset) {
    uint8_t beat    = beat8(bpm, timebase);
    uint8_t beatsin = sin8_t(beat + phase_offset);
    return lowest + scale8(beatsin, highest - lowest);
}

// ---- noise.cpp ----------------------------------------------------------------

static const uint8_t p[] = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240,
    21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,
    237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111,
    229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,
    209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217,
    226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,
    223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,
    108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241,
    81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,
    150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180, 151
};
#define P(x) p[(uint8_t)(x)]
#define EASE8(x) (ease8InOutQuad(x))
#define EASE16(x) (ease16InOutQuad(x))
#define LERP(a, b, u) lerp15by16(a, b, u)

static int16_t grad16(uint8_t hash, int16_t x, int16_t y, int16_t z) {
    hash      = hash & 15;
    int16_t u = hash < 8 ? x : y;
    int16_t v = hash < 4 ? y : hash == 12 || hash == 14 ? x : z;
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg15(u, v);
}
static int16_t grad16(uint8_t hash, int16_t x, int16_t y) {
    hash = hash & 7;
    int16_t u, v;
    if (hash < 4) { u = x; v = y; } else { u = y; v = x; }
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg15(u, v);
}
static int16_t grad16(uint8_t hash, int16_t x) {
    hash = hash & 15;
    int16_t u, v;
    if (hash > 8) { u = x; v = x; } else if (hash < 4) { u = x; v = 1; } else { u = 1; v = x; }
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg15(u, v);
}
static int8_t grad8(uint8_t hash, int8_t x, int8_t y, int8_t z) {
    hash &= 0xF;
    int8_t u = (hash & 8) ? y : x;
    int8_t v = hash < 4 ? y : hash == 12 || hash == 14 ? x : z;
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg7(u, v);
}
static int8_t grad8(uint8_t hash, int8_t x, int8_t y) {
    int8_t u, v;
    if (hash & 4) { u = y; v = x; } else { u = x; v = y; }
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg7(u, v);
}
static int8_t grad8(uint8_t hash, int8_t x) {
    int8_t u, v;
    if (hash & 8) {
        u = x; v = x;
    } else if (hash & 4) {
        u = 1; v = x;
    } else {
        u = x; v = 1;
    }
    if (hash & 1) u = -u;
    if (hash & 2) v = -v;
    return avg7(u, v);
}
static int8_t lerp7by8(int8_t a, int8_t b, fract8 frac) {
    if (b > a) {
        uint8_t delta = b - a;
        return a + scale8(delta, frac);
    }
    uint8_t delta = a - b;
    return a - scale8(delta, frac);
}

int16_t inoise16_raw(uint32_t x, uint32_t y, uint32_t z) {
    uint8_t  X = (x >> 16) & 0xFF, Y = (y >> 16) & 0xFF, Z = (z >> 16) & 0xFF;
    uint8_t  A = P(X) + Y, AA = P(A) + Z, AB = P(A + 1) + Z;
    uint8_t  Bv = P(X + 1) + Y, BA = P(Bv) + Z, BB = P(Bv + 1) + Z;
    uint16_t u = x & 0xFFFF, v = y & 0xFFFF, w = z & 0xFFFF;
    int16_t  xx = (u >> 1) & 0x7FFF, yy = (v >> 1) & 0x7FFF, zz = (w >> 1) & 0x7FFF;
    uint16_t N = 0x8000L;
    u = EASE16(u); v = EASE16(v); w = EASE16(w);
    int16_t X1 = LERP(grad16(P(AA), xx, yy, zz), grad16(P(BA), xx - N, yy, zz), u);
    int16_t X2 = LERP(grad16(P(AB), xx, yy - N, zz), grad16(P(BB), xx - N, yy - N, zz), u);
    int16_t X3 = LERP(grad16(P(AA + 1), xx, yy, zz - N), grad16(P(BA + 1), xx - N, yy, zz - N), u);
    int16_t X4 = LERP(grad16(P(AB + 1), xx, yy - N, zz - N), grad16(P(BB + 1), xx - N, yy - N, zz - N), u);
    int16_t Y1 = LERP(X1, X2, v);
    int16_t Y2 = LERP(X3, X4, v);
    return LERP(Y1, Y2, w);
}
uint16_t inoise16(uint32_t x, uint32_t y, uint32_t z) {
    int32_t  ans = inoise16_raw(x, y, z);
    ans          = ans + 19052L;
    uint32_t pan = ans;
    pan *= 440L;
    return (pan >> 8);
}
int16_t inoise16_raw(uint32_t x, uint32_t y) {
    uint8_t  X = x >> 16, Y = y >> 16;
    uint8_t  A = P(X) + Y, AA = P(A), AB = P(A + 1);
    uint8_t  Bv = P(X + 1) + Y, BA = P(Bv), BB = P(Bv + 1);
    uint16_t u = x & 0xFFFF, v = y & 0xFFFF;
    int16_t  xx = (u >> 1) & 0x7FFF, yy = (v >> 1) & 0x7FFF;
    uint16_t N = 0x8000L;
    u = EASE16(u); v = EASE16(v);
    int16_t X1 = LERP(grad16(P(AA), xx, yy), grad16(P(BA), xx - N, yy), u);
    int16_t X2 = LERP(grad16(P(AB), xx, yy - N), grad16(P(BB), xx - N, yy - N), u);
    return LERP(X1, X2, v);
}
uint16_t inoise16(uint32_t x, uint32_t y) {
    int32_t  ans = inoise16_raw(x, y);
    ans          = ans + 17308L;
    uint32_t pan = ans;
    pan *= 484L;
    return (pan >> 8);
}
int16_t inoise16_raw(uint32_t x) {
    uint8_t  X = x >> 16;
    uint8_t  A = P(X), AA = P(A), Bv = P(X + 1), BA = P(Bv);
    uint16_t u  = x & 0xFFFF;
    int16_t  xx = (u >> 1) & 0x7FFF;
    uint16_t N  = 0x8000L;
    u           = EASE16(u);
    return LERP(grad16(P(AA), xx), grad16(P(BA), xx - N), u);
}
uint16_t inoise16(uint32_t x) { return ((uint32_t)((int32_t)inoise16_raw(x) + 17308L)) << 1; }

int8_t inoise8_raw(uint16_t x, uint16_t y, uint16_t z) {
    uint8_t X = x >> 8, Y = y >> 8, Z = z >> 8;
    uint8_t A = P(X) + Y, AA = P(A) + Z, AB = P(A + 1) + Z;
    uint8_t Bv = P(X + 1) + Y, BA = P(Bv) + Z, BB = P(Bv + 1) + Z;
    uint8_t u = x, v = y, w = z;
    int8_t  xx = ((uint8_t)(x) >> 1) & 0x7F, yy = ((uint8_t)(y) >> 1) & 0x7F, zz = ((uint8_t)(z) >> 1) & 0x7F;
    uint8_t N = 0x80;
    u = EASE8(u); v = EASE8(v); w = EASE8(w);
    int8_t X1 = lerp7by8(grad8(P(AA), xx, yy, zz), grad8(P(BA), xx - N, yy, zz), u);
    int8_t X2 = lerp7by8(grad8(P(AB), xx, yy - N, zz), grad8(P(BB), xx - N, yy - N, zz), u);
    int8_t X3 = lerp7by8(grad8(P(AA + 1), xx, yy, zz - N), grad8(P(BA + 1), xx - N, yy, zz - N), u);
    int8_t X4 = lerp7by8(grad8(P(AB + 1), xx, yy - N, zz - N), grad8(P(BB + 1), xx - N, yy - N, zz - N), u);
    int8_t Y1 = lerp7by8(X1, X2, v);
    int8_t Y2 = lerp7by8(X3, X4, v);
    return lerp7by8(Y1, Y2, w);
}
uint8_t inoise8(uint16_t x, uint16_t y, uint16_t z) {
    int8_t n = inoise8_raw(x, y, z);
    n += 64;
    return qadd8(n, n);
}
int8_t inoise8_raw(uint16_t x, uint16_t y) {
    uint8_t X = x >> 8, Y = y >> 8;
    uint8_t A = P(X) + Y, AA = P(A), AB = P(A + 1);
    uint8_t Bv = P(X + 1) + Y, BA = P(Bv), BB = P(Bv + 1);
    uint8_t u = x, v = y;
    int8_t  xx = ((uint8_t)(x) >> 1) & 0x7F, yy = ((uint8_t)(y) >> 1) & 0x7F;
    uint8_t N = 0x80;
    u = EASE8(u); v = EASE8(v);
    int8_t X1 = lerp7by8(grad8(P(AA), xx, yy), grad8(P(BA), xx - N, yy), u);
    int8_t X2 = lerp7by8(grad8(P(AB), xx, yy - N), grad8(P(BB), xx - N, yy - N), u);
    return lerp7by8(X1, X2, v);
}
uint8_t inoise8(uint16_t x, uint16_t y) {
    int8_t n = inoise8_raw(x, y);
    n += 64;
    return qadd8(n, n);
}
int8_t inoise8_raw(uint16_t x) {
    uint8_t X = x >> 8;
    uint8_t A = P(X), AA = P(A), Bv = P(X + 1), BA = P(Bv);
    uint8_t u  = x;
    int8_t  xx = ((uint8_t)(x) >> 1) & 0x7F;
    uint8_t N  = 0x80;
    u          = EASE8(u);
    return lerp7by8(grad8(P(AA), xx), grad8(P(BA), xx - N), u);
}
uint8_t inoise8(uint16_t x) {
    int8_t n = inoise8_raw(x);
    n += 64;
    return qadd8(n, n);
}
#undef P

// ---- hsv2rgb.cpp / colorutils.cpp ---------------------------------------------

void hsv2rgb_rainbow(const CHSV& hsv, CRGB& rgb) {
    uint8_t hue = hsv.hue, sat = hsv.sat, val = hsv.val;
    uint8_t offset8 = (hue & 0x1F) << 3;
    uint8_t third   = scale8(offset8, (256 / 3));
    uint8_t r, g, b;
    if (!(hue & 0x80)) {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) {
                r = 255 - third; g = third; b = 0;
            } else {
                r = 171; g = 85 + third; b = 0;
            }
        } else {
            if (!(hue & 0x20)) {
                uint8_t twothirds = scale8(offset8, ((256 * 2) / 3));
                r = 171 - twothirds; g = 170 + third; b = 0;
            } else {
                r = 0; g = 255 - third; b = third;
            }
        }
    } else {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) {
                uint8_t twothirds = scale8(offset8, ((256 * 2) / 3));
                r = 0; g = 171 - twothirds; b = 85 + twothirds;
            } else {
                r = third; g = 0; b = 255 - third;
            }
        } else {
            if (!(hue & 0x20)) {
                r = 85 + third; g = 0; b = 171 - third;
            } else {
                r = 170 + third; g = 0; b = 85 - third;
            }
        }
    }
    if (sat != 255) {
        if (sat == 0) {
            r = 255; b = 255; g = 255;
        } else {
            uint8_t desat    = 255 - sat;
            desat            = scale8_video(desat, desat);
            uint8_t satscale = 255 - desat;
            r = scale8(r, satscale);
            g = scale8(g, satscale);
            b = scale8(b, satscale);
            r += desat;
            g += desat;
            b += desat;
        }
    }
    if (val != 255) {
        val = scale8_video(val, val);
        if (val == 0) {
            r = 0; g = 0; b = 0;
        } else {
            r = scale8(r, val);
            g = scale8(g, val);
            b = scale8(b, val);
        }
    }
    rgb.r = r;
    rgb.g = g;
    rgb.b = b;
}

void fill_solid(CRGB* leds, int numToFill, const CRGB& color) {
    for (int i = 0; i < numToFill; ++i) leds[i] = color;
}

void fill_gradient_RGB(CRGB* leds, uint16_t startpos, CRGB startcolor, uint16_t endpos, CRGB endcolor) {
    if (endpos < startpos) {
        std::swap(startpos, endpos);
        std::swap(startcolor, endcolor);
    }
    saccum87 rdistance87 = (endcolor.r - startcolor.r) << 7;
    saccum87 gdistance87 = (endcolor.g - startcolor.g) << 7;
    saccum87 bdistance87 = (endcolor.b - startcolor.b) << 7;
    uint16_t pixeldistance = endpos - startpos;
    int16_t  divisor       = pixeldistance ? pixeldistance : 1;
    saccum87 rdelta87 = rdistance87 / divisor;
    saccum87 gdelta87 = gdistance87 / divisor;
    saccum87 bdelta87 = bdistance87 / divisor;
    rdelta87 *= 2;
    gdelta87 *= 2;
    bdelta87 *= 2;
    uint16_t r88 = startcolor.r << 8, g88 = startcolor.g << 8, b88 = startcolor.b << 8;
    for (uint16_t i = startpos; i <= endpos; ++i) {
        leds[i] = CRGB(r88 >> 8, g88 >> 8, b88 >> 8);
        r88 += rdelta87;
        g88 += gdelta87;
        b88 += bdelta87;
    }
}
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2) { fill_gradient_RGB(leds, 0, c1, numLeds - 1, c2); }
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2, const CRGB& c3) {
    uint16_t half = (numLeds / 2), last = numLeds - 1;
    fill_gradient_RGB(leds, 0, c1, half, c2);
    fill_gradient_RGB(leds, half, c2, last, c3);
}
void fill_gradient_RGB(CRGB* leds, uint16_t numLeds, const CRGB& c1, const CRGB& c2, const CRGB& c3, const CRGB& c4) {
    uint16_t onethird = (numLeds / 3), twothirds = ((numLeds * 2) / 3), last = numLeds - 1;
    fill_gradient_RGB(leds, 0, c1, onethird, c2);
    fill_gradient_RGB(leds, onethird, c2, twothirds, c3);
    fill_gradient_RGB(leds, twothirds, c3, last, c4);
}

CRGB ColorFromPalette(const CRGBPalette16& pal, uint8_t index, uint8_t brightness, TBlendType blendType) {
    if (blendType == LINEARBLEND_NOWRAP) {
        index = map8(index, 0, 239);
    }
    uint8_t     hi4    = index >> 4;
    uint8_t     lo4    = index & 0x0F;
    const CRGB* entry  = &pal.entries[hi4];
    uint8_t     blend  = lo4 && (blendType != NOBLEND);
    uint8_t     red1   = entry->red;
    uint8_t     green1 = entry->green;
    uint8_t     blue1  = entry->blue;
    if (blend) {
        entry       = (hi4 == 15) ? &pal.entries[0] : entry + 1;
        uint8_t f2  = lo4 << 4;
        uint8_t f1  = 255 - f2;
        red1        = scale8(red1, f1) + scale8(entry->red, f2);
        green1      = scale8(green1, f1) + scale8(entry->green, f2);
        blue1       = scale8(blue1, f1) + scale8(entry->blue, f2);
    }
    if (brightness != 255) {
        if (brightness) {
            ++brightness;
            if (red1) red1 = scale8(red1, brightness) + 1;
            if (green1) green1 = scale8(green1, brightness) + 1;
            if (blue1) blue1 = scale8(blue1, brightness) + 1;
        } else {
            red1 = 0; green1 = 0; blue1 = 0;
        }
    }
    return CRGB(red1, green1, blue1);
}

CRGB HeatColor(uint8_t temperature) {
    CRGB    heatcolor;
    uint8_t t192     = scale8_video(temperature, 191);
    uint8_t heatramp = t192 & 0x3F;
    heatramp <<= 2;
    if (t192 & 0x80) {
        heatcolor.r = 255; heatcolor.g = 255; heatcolor.b = heatramp;
    } else if (t192 & 0x40) {
        heatcolor.r = 255; heatcolor.g = heatramp; heatcolor.b = 0;
    } else {
        heatcolor.r = heatramp; heatcolor.g = 0; heatcolor.b = 0;
    }
    return heatcolor;
}

CRGB& nblend(CRGB& existing, const CRGB& overlay, fract8 amountOfOverlay) {
    if (amountOfOverlay == 0) {
        return existing;
    }
    if (amountOfOverlay == 255) {
        existing = overlay;
        return existing;
    }
    existing.red   = blend8(existing.red, overlay.red, amountOfOverlay);
    existing.green = blend8(existing.green, overlay.green, amountOfOverlay);
    existing.blue  = blend8(existing.blue, overlay.blue, amountOfOverlay);
    return existing;
}
CRGB blend(const CRGB& p1, const CRGB& p2, fract8 amountOfP2) {
    CRGB nu(p1);
    nblend(nu, p2, amountOfP2);
    return nu;
}

void nblendPaletteTowardPalette(CRGBPalette16& current, CRGBPalette16& target, uint8_t maxChanges) {
    uint8_t* p1      = (uint8_t*)current.entries;
    uint8_t* p2      = (uint8_t*)target.entries;
    uint8_t  changes = 0;
    for (uint8_t i = 0; i < 48; ++i) {
        if (p1[i] == p2[i]) continue;
        if (p1[i] < p2[i]) { ++p1[i]; ++changes; }
        if (p1[i] > p2[i]) {
            --p1[i]; ++changes;
            if (p1[i] > p2[i]) --p1[i];
        }
        if (changes >= maxChanges) break;
    }
}

// ---- colors.cpp ------------------------------------------------------------------

uint32_t color_blend(uint32_t color1, uint32_t color2, uint16_t blend, bool b16) {
    if (blend == 0) return color1;
    unsigned blendmax = b16 ? 0xFFFF : 0xFF;
    if (blend == blendmax) return color2;
    unsigned shift = b16 ? 16 : 8;
    uint32_t w1 = W(color1), r1 = R(color1), g1 = G(color1), b1 = B(color1);
    uint32_t w2 = W(color2), r2 = R(color2), g2 = G(color2), b2 = B(color2);
    uint32_t w3 = ((w2 * blend) + (w1 * (blendmax - blend))) >> shift;
    uint32_t r3 = ((r2 * blend) + (r1 * (blendmax - blend))) >> shift;
    uint32_t g3 = ((g2 * blend) + (g1 * (blendmax - blend))) >> shift;
    uint32_t b3 = ((b2 * blend) + (b1 * (blendmax - blend))) >> shift;
    return RGBW32(r3, g3, b3, w3);
}

uint32_t color_add(uint32_t c1, uint32_t c2, bool fast) {
    if (c1 == BLACK) return c2;
    if (c2 == BLACK) return c1;
    if (fast) {
        return RGBW32(qadd8(R(c1), R(c2)), qadd8(G(c1), G(c2)), qadd8(B(c1), B(c2)), qadd8(W(c1), W(c2)));
    }
    uint32_t r = R(c1) + R(c2), g = G(c1) + G(c2), b = B(c1) + B(c2), w = W(c1) + W(c2);
    unsigned m = r;
    if (g > m) m = g;
    if (b > m) m = b;
    if (w > m) m = w;
    if (m < 256) return RGBW32(r, g, b, w);
    return RGBW32(r * 255 / m, g * 255 / m, b * 255 / m, w * 255 / m);
}

uint32_t color_fade(uint32_t c1, uint8_t amount, bool video) {
    if (c1 == BLACK || amount + video == 0) return BLACK;
    uint32_t r = R(c1), g = G(c1), b = B(c1), w = W(c1);
    uint32_t scale = amount;
    uint32_t scaledcolor;
    if (video) {
        scaledcolor = (((r * scale) >> 8) + ((r && scale) ? 1 : 0)) << 16;
        scaledcolor |= (((g * scale) >> 8) + ((g && scale) ? 1 : 0)) << 8;
        scaledcolor |= ((b * scale) >> 8) + ((b && scale) ? 1 : 0);
        scaledcolor |= (((w * scale) >> 8) + ((w && scale) ? 1 : 0)) << 24;
    } else {
        scaledcolor = ((r * scale) >> 8) << 16;
        scaledcolor |= ((g * scale) >> 8) << 8;
        scaledcolor |= (b * scale) >> 8;
        scaledcolor |= ((w * scale) >> 8) << 24;
    }
    return scaledcolor;
}

uint8_t get_random_wheel_index(uint8_t pos) {
    uint8_t r = 0, x = 0, y = 0, d = 0;
    while (d < 42) {
        r = random8();
        x = std::abs(pos - r);
        y = 255 - x;
        d = MIN(x, y);
    }
    return r;
}

static const uint8_t gammaT[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,
    3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   6,   7,   7,   7,   7,   8,   8,   8,   9,   9,
    9,   10,  10,  10,  11,  11,  11,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,
    21,  21,  22,  22,  23,  24,  24,  25,  25,  26,  27,  27,  28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  35,  36,  37,  38,
    39,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  50,  51,  52,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  66,  67,  68,  69,  70,  72,  73,  74,  75,  77,  78,  79,  81,  82,  83,  85,  86,  87,  89,  90,  92,  93,  95,  96,  98,
    99,  101, 102, 104, 105, 107, 109, 110, 112, 114, 115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142,
    144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 167, 169, 171, 173, 175, 177, 180, 182, 184, 186, 189, 191, 193, 196, 198,
    200, 203, 205, 208, 210, 213, 215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247, 249, 252, 255
};
uint8_t  gamma8(uint8_t b) { return gammaT[b]; }
uint32_t gamma32(uint32_t c) { return c; }  // WLED gamma-corrects colors; our strip never has

// ---- Segment (FX_fcn.cpp) ----------------------------------------------------------

// Heap guard: an effect asking for more falls back to mode_static, as WLED's
// does when MAX_SEGMENT_DATA runs out.  ponytail: one fixed cap, not a pool.
static constexpr size_t kMaxData = 4096;

bool Segment::allocateData(size_t len) {
    if (len == 0) return false;
    if (data && _dataLen >= len) {
        if (call == 0) memset(data, 0, len);
        return true;
    }
    deallocateData();
    if (len > kMaxData) return false;
    data = (uint8_t*)calloc(len, 1);
    if (!data) return false;
    _dataLen = len;
    return true;
}

void Segment::deallocateData() {
    free(data);
    data     = nullptr;
    _dataLen = 0;
}

void Segment::setPixelColor(int i, uint32_t col) {
    i &= 0xFFFF;  // WLED's virtual-strip index tag (indexToVStrip): one strip here
    if (i >= virtualLength()) return;
    _px[i] = col & 0xFFFFFF;  // no W channel at effect level
}

void Segment::setPixelColor(float i, uint32_t col, bool aa) {
    int vStrip = int(i / 10.0f);
    i -= int(i);
    if (i < 0.0f || i > 1.0f) return;
    float fC = i * (virtualLength() - 1);
    if (aa) {
        unsigned iL  = roundf(fC - 0.49f);
        unsigned iR  = roundf(fC + 0.49f);
        float    dL  = (fC - iL) * (fC - iL);
        float    dR  = (iR - fC) * (iR - fC);
        uint32_t cIL = getPixelColor(iL | (vStrip << 16));
        uint32_t cIR = getPixelColor(iR | (vStrip << 16));
        if (iR != iL) {
            cIL = color_blend(col, cIL, uint8_t(dL * 255.0f));
            setPixelColor(int(iL | (vStrip << 16)), cIL);
            cIR = color_blend(col, cIR, uint8_t(dR * 255.0f));
            setPixelColor(int(iR | (vStrip << 16)), cIR);
        } else {
            setPixelColor(int(iL | (vStrip << 16)), col);
        }
    } else {
        setPixelColor(int(roundf(fC)) | (vStrip << 16), col);
    }
}

uint32_t Segment::getPixelColor(int i) const {
    i &= 0xFFFF;
    if (i >= virtualLength()) return 0;
    return _px[i];
}

void Segment::fill(uint32_t c) {
    for (int i = 0; i < virtualLength(); i++) _px[i] = c & 0xFFFFFF;
}

void Segment::fade_out(uint8_t rate) {
    rate                 = (256 - rate) >> 1;
    const int mappedRate = 256 / (rate + 1);
    for (int i = 0; i < virtualLength(); i++) {
        uint32_t color = _px[i];
        if (color == colors[1]) continue;
        for (int b = 0; b < 32; b += 8) {
            uint8_t c2    = (colors[1] >> b);
            uint8_t c1    = (color >> b);
            int     delta = (c2 - c1) * mappedRate / 256;
            if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;
            color &= ~(0xFF << b);
            color |= ((c1 + delta) & 0xFF) << b;
        }
        _px[i] = color & 0xFFFFFF;
    }
}

void Segment::fadeToBlackBy(uint8_t fadeBy) {
    if (fadeBy == 0) return;
    for (int i = 0; i < virtualLength(); i++) _px[i] = color_fade(_px[i], 255 - fadeBy);
}

void Segment::blur(uint8_t blur_amount, bool smear) {
    if (blur_amount == 0) return;
    if (is2D()) {
        blur2D(blur_amount, smear);
        return;
    }
    uint8_t  keep      = smear ? 255 : 255 - blur_amount;
    uint8_t  seep      = blur_amount >> (1 + smear);
    unsigned vlength   = virtualLength();
    uint32_t carryover = BLACK;
    uint32_t lastnew   = BLACK;
    uint32_t last      = BLACK;
    uint32_t curnew    = BLACK;
    for (unsigned i = 0; i < vlength; i++) {
        uint32_t cur  = getPixelColor(i);
        uint32_t part = color_fade(cur, seep);
        curnew        = color_fade(cur, keep);
        if (i > 0) {
            if (carryover) curnew = color_add(curnew, carryover, true);
            uint32_t prev = color_add(lastnew, part, true);
            if (last != prev) setPixelColor(int(i - 1), prev);
        } else {
            setPixelColor(int(i), curnew);
        }
        lastnew   = curnew;
        last      = cur;
        carryover = part;
    }
    setPixelColor(int(vlength - 1), curnew);
}

uint32_t Segment::color_wheel(uint8_t pos) const {
    if (palette) return color_from_palette(pos, false, true, 0);
    pos = 255 - pos;
    if (pos < 85) {
        return RGBW32((255 - pos * 3), 0, (pos * 3), 0);
    } else if (pos < 170) {
        pos -= 85;
        return RGBW32(0, (pos * 3), (255 - pos * 3), 0);
    }
    pos -= 170;
    return RGBW32((pos * 3), (255 - pos * 3), 0, 0);
}

uint32_t Segment::color_from_palette(uint16_t i, bool mapping, bool wrap, uint8_t mcol, uint8_t pbri) const {
    uint32_t color = gamma32(currentColor(mcol));
    if (palette == 0 && mcol < NUM_COLORS) return (pbri == 255) ? color : color_fade(color, pbri, true);
    unsigned paletteIndex = i;
    if (mapping && virtualLength() > 1) paletteIndex = (i * 255) / (virtualLength() - 1);
    if (!wrap && strip.paletteBlend != 3) paletteIndex = scale8(paletteIndex, 240);
    CRGB fastled_col = ColorFromPalette(_currentPalette, paletteIndex, pbri, (strip.paletteBlend == 3) ? NOBLEND : LINEARBLEND);
    return RGBW32(fastled_col.r, fastled_col.g, fastled_col.b, 0);
}

// ---- Segment 2D (FX_2Dfcn.cpp) -----------------------------------------------------

void Segment::setPixelColorXY(int x, int y, uint32_t col) {
    if ((unsigned)x >= _w || (unsigned)y >= _h || x < 0 || y < 0) return;
    _px[x + y * _w] = col & 0xFFFFFF;
}

void Segment::setPixelColorXY(float x, float y, uint32_t col, bool aa) {
    if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return;
    const unsigned cols = _w, rows = _h;
    float          fX = x * (cols - 1);
    float          fY = y * (rows - 1);
    if (aa) {
        unsigned xL    = roundf(fX - 0.49f);
        unsigned xR    = roundf(fX + 0.49f);
        unsigned yT    = roundf(fY - 0.49f);
        unsigned yB    = roundf(fY + 0.49f);
        float    dL    = (fX - xL) * (fX - xL);
        float    dR    = (xR - fX) * (xR - fX);
        float    dT    = (fY - yT) * (fY - yT);
        float    dB    = (yB - fY) * (yB - fY);
        uint32_t cXLYT = getPixelColorXY(xL, yT);
        uint32_t cXRYT = getPixelColorXY(xR, yT);
        uint32_t cXLYB = getPixelColorXY(xL, yB);
        uint32_t cXRYB = getPixelColorXY(xR, yB);
        if (xL != xR && yT != yB) {
            setPixelColorXY(int(xL), int(yT), color_blend(col, cXLYT, uint8_t(sqrtf(dL * dT) * 255.0f)));
            setPixelColorXY(int(xR), int(yT), color_blend(col, cXRYT, uint8_t(sqrtf(dR * dT) * 255.0f)));
            setPixelColorXY(int(xL), int(yB), color_blend(col, cXLYB, uint8_t(sqrtf(dL * dB) * 255.0f)));
            setPixelColorXY(int(xR), int(yB), color_blend(col, cXRYB, uint8_t(sqrtf(dR * dB) * 255.0f)));
        } else if (xR != xL && yT == yB) {
            setPixelColorXY(int(xR), int(yT), color_blend(col, cXLYT, uint8_t(dL * 255.0f)));
            setPixelColorXY(int(xR), int(yT), color_blend(col, cXRYT, uint8_t(dR * 255.0f)));
        } else if (xR == xL && yT != yB) {
            setPixelColorXY(int(xR), int(yT), color_blend(col, cXLYT, uint8_t(dT * 255.0f)));
            setPixelColorXY(int(xL), int(yB), color_blend(col, cXLYB, uint8_t(dB * 255.0f)));
        } else {
            setPixelColorXY(int(xL), int(yT), col);
        }
    } else {
        setPixelColorXY(int(uint16_t(roundf(fX))), int(uint16_t(roundf(fY))), col);
    }
}

uint32_t Segment::getPixelColorXY(int x, int y) const {
    if ((unsigned)x >= _w || (unsigned)y >= _h || x < 0 || y < 0) return 0;
    return _px[x + y * _w];
}

void Segment::blurRow(uint32_t row, fract8 blur_amount, bool smear) {
    if (blur_amount == 0 || row >= _h) return;
    const unsigned cols      = _w;
    uint8_t        keep      = smear ? 255 : 255 - blur_amount;
    uint8_t        seep      = blur_amount >> 1;
    uint32_t       carryover = BLACK, lastnew = BLACK, last = BLACK, curnew = BLACK;
    for (unsigned x = 0; x < cols; x++) {
        uint32_t cur  = getPixelColorXY(x, row);
        uint32_t part = color_fade(cur, seep);
        curnew        = color_fade(cur, keep);
        if (x > 0) {
            if (carryover) curnew = color_add(curnew, carryover, true);
            uint32_t prev = color_add(lastnew, part, true);
            if (last != prev) setPixelColorXY(int(x - 1), int(row), prev);
        } else {
            setPixelColorXY(int(x), int(row), curnew);
        }
        lastnew   = curnew;
        last      = cur;
        carryover = part;
    }
    setPixelColorXY(int(cols - 1), int(row), curnew);
}

void Segment::blurCol(uint32_t col, fract8 blur_amount, bool smear) {
    if (blur_amount == 0 || col >= _w) return;
    const unsigned rows      = _h;
    uint8_t        keep      = smear ? 255 : 255 - blur_amount;
    uint8_t        seep      = blur_amount >> 1;
    uint32_t       carryover = BLACK, lastnew = BLACK, last = BLACK, curnew = BLACK;
    for (unsigned y = 0; y < rows; y++) {
        uint32_t cur  = getPixelColorXY(col, y);
        uint32_t part = color_fade(cur, seep);
        curnew        = color_fade(cur, keep);
        if (y > 0) {
            if (carryover) curnew = color_add(curnew, carryover, true);
            uint32_t prev = color_add(lastnew, part, true);
            if (last != prev) setPixelColorXY(int(col), int(y - 1), prev);
        } else {
            setPixelColorXY(int(col), int(y), curnew);
        }
        lastnew   = curnew;
        last      = cur;
        carryover = part;
    }
    setPixelColorXY(int(col), int(rows - 1), curnew);
}

void Segment::blur2D(uint8_t blur_amount, bool smear) {
    if (blur_amount == 0) return;
    const unsigned cols = _w, rows = _h;
    const uint8_t  keep = smear ? 255 : 255 - blur_amount;
    const uint8_t  seep = blur_amount >> (1 + smear);
    uint32_t       lastnew = BLACK, last = BLACK;
    for (unsigned row = 0; row < rows; row++) {
        uint32_t carryover = BLACK, curnew = BLACK;
        for (unsigned x = 0; x < cols; x++) {
            uint32_t cur  = getPixelColorXY(x, row);
            uint32_t part = color_fade(cur, seep);
            curnew        = color_fade(cur, keep);
            if (x > 0) {
                if (carryover) curnew = color_add(curnew, carryover, true);
                uint32_t prev = color_add(lastnew, part, true);
                if (last != prev) setPixelColorXY(int(x - 1), int(row), prev);
            } else {
                setPixelColorXY(int(x), int(row), curnew);
            }
            lastnew   = curnew;
            last      = cur;
            carryover = part;
        }
        setPixelColorXY(int(cols - 1), int(row), curnew);
    }
    for (unsigned col = 0; col < cols; col++) {
        uint32_t carryover = BLACK, curnew = BLACK;
        for (unsigned y = 0; y < rows; y++) {
            uint32_t cur  = getPixelColorXY(col, y);
            uint32_t part = color_fade(cur, seep);
            curnew        = color_fade(cur, keep);
            if (y > 0) {
                if (carryover) curnew = color_add(curnew, carryover, true);
                uint32_t prev = color_add(lastnew, part, true);
                if (last != prev) setPixelColorXY(int(col), int(y - 1), prev);
            } else {
                setPixelColorXY(int(col), int(y), curnew);
            }
            lastnew   = curnew;
            last      = cur;
            carryover = part;
        }
        setPixelColorXY(int(col), int(rows - 1), curnew);
    }
}

void Segment::moveX(int8_t delta, bool wrap) {
    const int cols = _w, rows = _h;
    if (!delta || std::abs(delta) >= cols) return;
    uint32_t newPxCol[256];  // matrix sides are <= 256
    for (int y = 0; y < rows; y++) {
        if (delta > 0) {
            for (int x = 0; x < cols - delta; x++) newPxCol[x] = getPixelColorXY((x + delta), y);
            for (int x = cols - delta; x < cols; x++) newPxCol[x] = getPixelColorXY(wrap ? (x + delta) - cols : x, y);
        } else {
            for (int x = cols - 1; x >= -delta; x--) newPxCol[x] = getPixelColorXY((x + delta), y);
            for (int x = -delta - 1; x >= 0; x--) newPxCol[x] = getPixelColorXY(wrap ? (x + delta) + cols : x, y);
        }
        for (int x = 0; x < cols; x++) setPixelColorXY(x, y, newPxCol[x]);
    }
}

void Segment::moveY(int8_t delta, bool wrap) {
    const int cols = _w, rows = _h;
    if (!delta || std::abs(delta) >= rows) return;
    uint32_t newPxCol[256];
    for (int x = 0; x < cols; x++) {
        if (delta > 0) {
            for (int y = 0; y < rows - delta; y++) newPxCol[y] = getPixelColorXY(x, (y + delta));
            for (int y = rows - delta; y < rows; y++) newPxCol[y] = getPixelColorXY(x, wrap ? (y + delta) - rows : y);
        } else {
            for (int y = rows - 1; y >= -delta; y--) newPxCol[y] = getPixelColorXY(x, (y + delta));
            for (int y = -delta - 1; y >= 0; y--) newPxCol[y] = getPixelColorXY(x, wrap ? (y + delta) + rows : y);
        }
        for (int y = 0; y < rows; y++) setPixelColorXY(x, y, newPxCol[y]);
    }
}

void Segment::move(uint8_t dir, uint8_t delta, bool wrap) {
    if (delta == 0) return;
    switch (dir) {
        case 0: moveX(delta, wrap); break;
        case 1: moveX(delta, wrap); moveY(delta, wrap); break;
        case 2: moveY(delta, wrap); break;
        case 3: moveX(-delta, wrap); moveY(delta, wrap); break;
        case 4: moveX(-delta, wrap); break;
        case 5: moveX(-delta, wrap); moveY(-delta, wrap); break;
        case 6: moveY(-delta, wrap); break;
        case 7: moveX(delta, wrap); moveY(-delta, wrap); break;
    }
}

void Segment::drawCircle(uint16_t cx, uint16_t cy, uint8_t radius, uint32_t col, bool soft) {
    if (radius == 0) return;
    if (soft) {
        int      rsq     = radius * radius;
        int      x       = 0;
        int      y       = radius;
        unsigned oldFade = 0;
        while (x < y) {
            float    yf   = sqrtf(float(rsq - x * x));
            unsigned fade = float(0xFFFF) * (ceilf(yf) - yf);
            if (oldFade > fade) y--;
            oldFade = fade;
            setPixelColorXY(cx + x, cy + y, color_blend(col, getPixelColorXY(cx + x, cy + y), fade, true));
            setPixelColorXY(cx - x, cy + y, color_blend(col, getPixelColorXY(cx - x, cy + y), fade, true));
            setPixelColorXY(cx + x, cy - y, color_blend(col, getPixelColorXY(cx + x, cy - y), fade, true));
            setPixelColorXY(cx - x, cy - y, color_blend(col, getPixelColorXY(cx - x, cy - y), fade, true));
            setPixelColorXY(cx + y, cy + x, color_blend(col, getPixelColorXY(cx + y, cy + x), fade, true));
            setPixelColorXY(cx - y, cy + x, color_blend(col, getPixelColorXY(cx - y, cy + x), fade, true));
            setPixelColorXY(cx + y, cy - x, color_blend(col, getPixelColorXY(cx + y, cy - x), fade, true));
            setPixelColorXY(cx - y, cy - x, color_blend(col, getPixelColorXY(cx - y, cy - x), fade, true));
            setPixelColorXY(cx + x, cy + y - 1, color_blend(getPixelColorXY(cx + x, cy + y - 1), col, fade, true));
            setPixelColorXY(cx - x, cy + y - 1, color_blend(getPixelColorXY(cx - x, cy + y - 1), col, fade, true));
            setPixelColorXY(cx + x, cy - y + 1, color_blend(getPixelColorXY(cx + x, cy - y + 1), col, fade, true));
            setPixelColorXY(cx - x, cy - y + 1, color_blend(getPixelColorXY(cx - x, cy - y + 1), col, fade, true));
            setPixelColorXY(cx + y - 1, cy + x, color_blend(getPixelColorXY(cx + y - 1, cy + x), col, fade, true));
            setPixelColorXY(cx - y + 1, cy + x, color_blend(getPixelColorXY(cx - y + 1, cy + x), col, fade, true));
            setPixelColorXY(cx + y - 1, cy - x, color_blend(getPixelColorXY(cx + y - 1, cy - x), col, fade, true));
            setPixelColorXY(cx - y + 1, cy - x, color_blend(getPixelColorXY(cx - y + 1, cy - x), col, fade, true));
            x++;
        }
    } else {
        int d = 3 - (2 * radius);
        int y = radius, x = 0;
        while (y >= x) {
            setPixelColorXY(cx + x, cy + y, col);
            setPixelColorXY(cx - x, cy + y, col);
            setPixelColorXY(cx + x, cy - y, col);
            setPixelColorXY(cx - x, cy - y, col);
            setPixelColorXY(cx + y, cy + x, col);
            setPixelColorXY(cx - y, cy + x, col);
            setPixelColorXY(cx + y, cy - x, col);
            setPixelColorXY(cx - y, cy - x, col);
            x++;
            if (d > 0) {
                y--;
                d += 4 * (x - y) + 10;
            } else {
                d += 4 * x + 6;
            }
        }
    }
}

void Segment::fillCircle(uint16_t cx, uint16_t cy, uint8_t radius, uint32_t col, bool soft) {
    if (radius == 0) return;
    if (soft) drawCircle(cx, cy, radius, col, soft);
    const int cols = _w, rows = _h;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius && int(cx) + x >= 0 && int(cy) + y >= 0 && int(cx) + x < cols && int(cy) + y < rows)
                setPixelColorXY(cx + x, cy + y, col);
        }
    }
}

void Segment::drawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint32_t c, bool soft) {
    const int cols = _w, rows = _h;
    if (x0 >= cols || x1 >= cols || y0 >= rows || y1 >= rows) return;
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    if (dx + dy == 0) {
        setPixelColorXY(int(x0), int(y0), c);
        return;
    }
    if (soft) {
        const bool steep = dy > dx;
        if (steep) {
            std::swap(x0, y0);
            std::swap(x1, y1);
        }
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        float gradient   = x1 - x0 == 0 ? 1.0f : float(y1 - y0) / float(x1 - x0);
        float intersectY = y0;
        for (int x = x0; x <= x1; x++) {
            unsigned keep = float(0xFFFF) * (intersectY - int(intersectY));
            unsigned seep = 0xFFFF - keep;
            int      y    = int(intersectY);
            if (steep) std::swap(x, y);
            setPixelColorXY(x, y, color_blend(c, getPixelColorXY(x, y), keep, true));
            setPixelColorXY(x + int(steep), y + int(!steep), color_blend(c, getPixelColorXY(x + int(steep), y + int(!steep)), seep, true));
            intersectY += gradient;
            if (steep) std::swap(x, y);
        }
    } else {
        int err = (dx > dy ? dx : -dy) / 2;
        for (;;) {
            setPixelColorXY(int(x0), int(y0), c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = err;
            if (e2 > -dx) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dy) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

#define WU_WEIGHT(a, b) ((uint8_t)(((a) * (b) + (a) + (b)) >> 8))
void Segment::wu_pixel(uint32_t x, uint32_t y, CRGB c) {
    unsigned xx = x & 0xff, yy = y & 0xff, ix = 255 - xx, iy = 255 - yy;
    uint8_t  wu[4] = { WU_WEIGHT(ix, iy), WU_WEIGHT(xx, iy), WU_WEIGHT(ix, yy), WU_WEIGHT(xx, yy) };
    for (int i = 0; i < 4; i++) {
        int  wu_x   = (x >> 8) + (i & 1);
        int  wu_y   = (y >> 8) + ((i >> 1) & 1);
        CRGB led    = getPixelColorXY(wu_x, wu_y);
        CRGB oldLed = led;
        led.r       = qadd8(led.r, c.r * wu[i] >> 8);
        led.g       = qadd8(led.g, c.g * wu[i] >> 8);
        led.b       = qadd8(led.b, c.b * wu[i] >> 8);
        if (led != oldLed) setPixelColorXY(wu_x, wu_y, led);
    }
}
#undef WU_WEIGHT

}  // namespace wled
