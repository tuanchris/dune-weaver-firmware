// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_c.ts; tools/fx_parity.py checks them.
//
// Batch c: Fire Flicker, Loading, Rolling Balls, Fairy, Two Dots, Fairytwinkle,
// Running Dual, Chase 3, Tri Wipe, Tri Fade, ICU, Stream 2, Oscillate.
#include "WledFx.h"

#ifndef SPEED_FORMULA_L
#    define SPEED_FORMULA_L (5U + (50U * (255U - SEGMENT.speed)) / SEGLEN)
#endif

namespace wled {

// ---- Running Dual -------------------------------------------------------------
static uint8_t sin_gap(uint16_t in) {
    if (in & 0x100) return 0;
    return sin8_t(in + 192);  // correct phase shift of sine so that it starts and stops at 0
}

static uint16_t running_base(bool saw, bool dual = false) {
    unsigned x_scale = SEGMENT.intensity >> 2;
    uint32_t counter = (strip.now * SEGMENT.speed) >> 9;

    for (int i = 0; i < SEGLEN; i++) {
        unsigned a = i * x_scale - counter;
        if (saw) {
            a &= 0xFF;
            if (a < 16) {
                a = 192 + a * 8;
            } else {
                a = map(a, 16, 255, 64, 192);
            }
            a = 255 - a;
        }
        uint8_t  s  = dual ? sin_gap(a) : sin8_t(a);
        uint32_t ca = color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0), s);
        if (dual) {
            unsigned b  = (SEGLEN - 1 - i) * x_scale - counter;
            uint8_t  t  = sin_gap(b);
            uint32_t cb = color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 2), t);
            ca          = color_blend(ca, cb, 127);
        }
        SEGMENT.setPixelColor(i, ca);
    }

    return FRAMETIME;
}

uint16_t mode_running_dual(void) {
    return running_base(false, true);
}

// ---- Fire Flicker -------------------------------------------------------------
uint16_t mode_fire_flicker(void) {
    uint32_t cycleTime = 40 + (255 - SEGMENT.speed);
    uint32_t it        = strip.now / cycleTime;
    if (SEGENV.step == it) return FRAMETIME;

    byte w   = (SEGCOLOR(0) >> 24);
    byte r   = (SEGCOLOR(0) >> 16);
    byte g   = (SEGCOLOR(0) >> 8);
    byte b   = (SEGCOLOR(0));
    byte lum = (SEGMENT.palette == 0) ? MAX(w, MAX(r, MAX(g, b))) : 255;
    lum /= (((256 - SEGMENT.intensity) / 16) + 1);
    for (int i = 0; i < SEGLEN; i++) {
        byte flicker = random8(lum);
        if (SEGMENT.palette == 0) {
            SEGMENT.setPixelColor(i, MAX(r - flicker, 0), MAX(g - flicker, 0), MAX(b - flicker, 0), MAX(w - flicker, 0));
        } else {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0, 255 - flicker));
        }
    }

    SEGENV.step = it;
    return FRAMETIME;
}

// ---- Loading ------------------------------------------------------------------
static uint16_t gradient_base(bool loading) {
    if (SEGLEN == 1) return mode_static();
    uint16_t counter = strip.now * ((SEGMENT.speed >> 2) + 1);
    uint16_t pp      = (counter * SEGLEN) >> 16;
    if (SEGENV.call == 0) pp = 0;
    int val;  //0 = sec 1 = pri
    // WLED's precedence: (1 + loading) ? ... : ..., i.e. always intensity/2
    int brd = 1 + loading ? SEGMENT.intensity / 2 : SEGMENT.intensity / 4;
    //if (brd < 1) brd = 1;
    int p1 = pp - SEGLEN;
    int p2 = pp + SEGLEN;

    for (int i = 0; i < SEGLEN; i++) {
        if (loading) {
            val = abs(((i > pp) ? p2 : pp) - i);
        } else {
            val = min(abs(pp - i), min(abs(p1 - i), abs(p2 - i)));
        }
        val = (brd > val) ? (val * 255) / brd : 255;
        SEGMENT.setPixelColor(i, color_blend(SEGCOLOR(0), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1), val));
    }

    return FRAMETIME;
}

uint16_t mode_loading(void) {
    return gradient_base(true);
}

// ---- Two Dots -----------------------------------------------------------------
static uint16_t police_base(uint32_t color1, uint32_t color2) {
    if (SEGLEN == 1) return mode_static();
    unsigned delay  = 1 + (FRAMETIME << 3) / SEGLEN;  // longer segments should change faster
    uint32_t it     = strip.now / map(SEGMENT.speed, 0, 255, delay << 4, delay);
    unsigned offset = it % SEGLEN;

    unsigned width = ((SEGLEN * (SEGMENT.intensity + 1)) >> 9);  //max width is half the strip
    if (!width) width = 1;
    for (unsigned i = 0; i < width; i++) {
        unsigned indexR = (offset + i) % SEGLEN;
        unsigned indexB = (offset + i + (SEGLEN >> 1)) % SEGLEN;
        SEGMENT.setPixelColor(indexR, color1);
        SEGMENT.setPixelColor(indexB, color2);
    }
    return FRAMETIME;
}

uint16_t mode_two_dots(void) {
    if (!SEGMENT.check2) SEGMENT.fill(SEGCOLOR(2));
    uint32_t color2 = (SEGCOLOR(1) == SEGCOLOR(2)) ? SEGCOLOR(0) : SEGCOLOR(1);
    return police_base(SEGCOLOR(0), color2);
}

// ---- Fairy / Fairytwinkle -------------------------------------------------------
//4 bytes
typedef struct Flasher {
    uint16_t stateStart;
    uint8_t  stateDur;
    bool     stateOn;
} flasher;

static constexpr unsigned FLASHERS_PER_ZONE = 6;
static constexpr unsigned MAX_SHIMMER       = 92;

uint16_t mode_fairy(void) {
    //set every pixel to a 'random' color from palette (using seed so it doesn't change between frames)
    uint16_t PRNG16 = 5100 + strip.getCurrSegmentId();
    for (int i = 0; i < SEGLEN; i++) {
        PRNG16 = (uint16_t)(PRNG16 * 2053) + 1384;  //next 'random' number
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(PRNG16 >> 8, false, false, 0));
    }

    //amount of flasher pixels depending on intensity (0: none, 255: every LED)
    if (SEGMENT.intensity == 0) return FRAMETIME;
    unsigned flasherDistance = ((255 - SEGMENT.intensity) / 28) + 1;  //1-10
    unsigned numFlashers     = (SEGLEN / flasherDistance) + 1;

    unsigned dataSize = sizeof(flasher) * numFlashers;
    if (!SEGENV.allocateData(dataSize)) return FRAMETIME;  //allocation failed
    Flasher* flashers = reinterpret_cast<Flasher*>(SEGENV.data);
    unsigned now16    = strip.now & 0xFFFF;

    //Up to 11 flashers in one brightness zone, afterwards a new zone for every 6 flashers
    unsigned zones = numFlashers / FLASHERS_PER_ZONE;
    if (!zones) zones = 1;
    unsigned flashersInZone = numFlashers / zones;
    uint8_t  flasherBri[FLASHERS_PER_ZONE * 2 - 1];

    for (unsigned z = 0; z < zones; z++) {
        unsigned flasherBriSum = 0;
        unsigned firstFlasher  = z * flashersInZone;
        if (z == zones - 1) flashersInZone = numFlashers - (flashersInZone * (zones - 1));

        for (unsigned f = firstFlasher; f < firstFlasher + flashersInZone; f++) {
            unsigned stateTime = uint16_t(now16 - flashers[f].stateStart);
            //random on/off time reached, switch state
            if (stateTime > flashers[f].stateDur * 10) {
                flashers[f].stateOn = !flashers[f].stateOn;
                if (flashers[f].stateOn) {
                    flashers[f].stateDur = 12 + random8(12 + ((255 - SEGMENT.speed) >> 2));  //*10, 250ms to 1250ms
                } else {
                    flashers[f].stateDur = 20 + random8(6 + ((255 - SEGMENT.speed) >> 2));  //*10, 250ms to 1250ms
                }
                //flashers[f].stateDur = 51 + random8(2 + ((255 - SEGMENT.speed) >> 1));
                flashers[f].stateStart = now16;
                if (stateTime < 255) {
                    flashers[f].stateStart -= 255 - stateTime;  //start early to get correct bri
                    flashers[f].stateDur += 26 - stateTime / 10;
                    stateTime = 255 - stateTime;
                } else {
                    stateTime = 0;
                }
            }
            if (stateTime > 255) stateTime = 255;  //for flasher brightness calculation, fades in first 255 ms of state
            //flasherBri[f - firstFlasher] = (flashers[f].stateOn) ? 255-SEGMENT.gamma8((510 - stateTime) >> 1) : SEGMENT.gamma8((510 - stateTime) >> 1);
            flasherBri[f - firstFlasher] = (flashers[f].stateOn) ? stateTime : 255 - (stateTime >> 0);
            flasherBriSum += flasherBri[f - firstFlasher];
        }
        //dim factor, to create "shimmer" as other pixels get less voltage if a lot of flashers are on
        unsigned avgFlasherBri = flasherBriSum / flashersInZone;
        unsigned globalPeakBri = 255 - ((avgFlasherBri * MAX_SHIMMER) >> 8);  //183-255, suitable for 1/5th of LEDs flashers

        for (unsigned f = firstFlasher; f < firstFlasher + flashersInZone; f++) {
            unsigned bri        = (flasherBri[f - firstFlasher] * globalPeakBri) / 255;
            PRNG16              = (uint16_t)(PRNG16 * 2053) + 1384;  //next 'random' number
            unsigned flasherPos = f * flasherDistance;
            SEGMENT.setPixelColor(flasherPos, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(PRNG16 >> 8, false, false, 0), bri));
            for (unsigned i = flasherPos + 1; i < flasherPos + flasherDistance && i < SEGLEN; i++) {
                PRNG16 = (uint16_t)(PRNG16 * 2053) + 1384;  //next 'random' number
                SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(PRNG16 >> 8, false, false, 0, globalPeakBri));
            }
        }
    }
    return FRAMETIME;
}

uint16_t mode_fairytwinkle(void) {
    unsigned dataSize = sizeof(flasher) * SEGLEN;
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed
    Flasher* flashers = reinterpret_cast<Flasher*>(SEGENV.data);
    unsigned now16    = strip.now & 0xFFFF;
    uint16_t PRNG16   = 5100 + strip.getCurrSegmentId();

    unsigned riseFallTime = 400 + (255 - SEGMENT.speed) * 3;
    unsigned maxDur       = riseFallTime / 100 + ((255 - SEGMENT.intensity) >> 2) + 13 + ((255 - SEGMENT.intensity) >> 1);

    for (int f = 0; f < SEGLEN; f++) {
        uint16_t stateTime = now16 - flashers[f].stateStart;
        //random on/off time reached, switch state
        if (stateTime > flashers[f].stateDur * 100) {
            flashers[f].stateOn = !flashers[f].stateOn;
            bool init           = !flashers[f].stateDur;
            if (flashers[f].stateOn) {
                flashers[f].stateDur = riseFallTime / 100 + ((255 - SEGMENT.intensity) >> 2) + random8(12 + ((255 - SEGMENT.intensity) >> 1)) + 1;
            } else {
                flashers[f].stateDur = riseFallTime / 100 + random8(3 + ((255 - SEGMENT.speed) >> 6)) + 1;
            }
            flashers[f].stateStart = now16;
            stateTime              = 0;
            if (init) {
                flashers[f].stateStart -= riseFallTime;  //start lit
                flashers[f].stateDur = riseFallTime / 100 + random8(12 + ((255 - SEGMENT.intensity) >> 1)) + 5;  //fire up a little quicker
                stateTime            = riseFallTime;
            }
        }
        if (flashers[f].stateOn && flashers[f].stateDur > maxDur) flashers[f].stateDur = maxDur;  //react more quickly on intensity change
        if (stateTime > riseFallTime) stateTime = riseFallTime;  //for flasher brightness calculation, fades in first 255 ms of state
        unsigned fadeprog   = 255 - ((stateTime * 255) / riseFallTime);
        unsigned flasherBri = (flashers[f].stateOn) ? 255 - gamma8(fadeprog) : gamma8(fadeprog);
        unsigned lastR      = PRNG16;
        unsigned diff       = 0;
        while (diff < 0x4000) {                         //make sure colors of two adjacent LEDs differ enough
            PRNG16 = (uint16_t)(PRNG16 * 2053) + 1384;  //next 'random' number
            diff   = (PRNG16 > lastR) ? PRNG16 - lastR : lastR - PRNG16;
        }
        SEGMENT.setPixelColor(f, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(PRNG16 >> 8, false, false, 0), flasherBri));
    }
    return FRAMETIME;
}

// ---- Chase 3 ------------------------------------------------------------------
static uint16_t tricolor_chase(uint32_t color1, uint32_t color2) {
    uint32_t cycleTime = 50 + ((255 - SEGMENT.speed) << 1);
    uint32_t it        = strip.now / cycleTime;   // iterator
    unsigned width     = (1 + (SEGMENT.intensity >> 4));  // value of 1-16 for each colour
    unsigned index     = it % (width * 3);

    for (int i = 0; i < SEGLEN; i++, index++) {
        if (index > (width * 3) - 1) index = 0;

        uint32_t color = color1;
        if (index > (width << 1) - 1)
            color = SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1);
        else if (index > width - 1)
            color = color2;

        SEGMENT.setPixelColor(SEGLEN - i - 1, color);
    }
    return FRAMETIME;
}

uint16_t mode_tricolor_chase(void) {
    return tricolor_chase(SEGCOLOR(2), SEGCOLOR(0));
}

// ---- ICU ----------------------------------------------------------------------
uint16_t mode_icu(void) {
    unsigned dest  = SEGENV.step & 0xFFFF;
    unsigned space = (SEGMENT.intensity >> 3) + 2;

    if (!SEGMENT.check2) SEGMENT.fill(SEGCOLOR(1));

    byte     pindex = map(dest, 0, SEGLEN - SEGLEN / space, 0, 255);
    uint32_t col    = SEGMENT.color_from_palette(pindex, false, false, 0);

    SEGMENT.setPixelColor(dest, col);
    SEGMENT.setPixelColor(dest + SEGLEN / space, col);

    if (SEGENV.aux0 == dest) {  // pause between eye movements
        if (random8(6) == 0) {  // blink once in a while
            SEGMENT.setPixelColor(dest, SEGCOLOR(1));
            SEGMENT.setPixelColor(dest + SEGLEN / space, SEGCOLOR(1));
            return 200;
        }
        SEGENV.aux0 = random16(SEGLEN - SEGLEN / space);
        return 1000 + random16(2000);
    }

    if (SEGENV.aux0 > SEGENV.step) {
        SEGENV.step++;
        dest++;
    } else if (SEGENV.aux0 < SEGENV.step) {
        SEGENV.step--;
        dest--;
    }

    SEGMENT.setPixelColor(dest, col);
    SEGMENT.setPixelColor(dest + SEGLEN / space, col);

    return SPEED_FORMULA_L;
}

// ---- Tri Wipe -----------------------------------------------------------------
uint16_t mode_tricolor_wipe(void) {
    uint32_t cycleTime = 1000 + (255 - SEGMENT.speed) * 200;
    uint32_t perc      = strip.now % cycleTime;
    unsigned prog      = (perc * 65535) / cycleTime;
    unsigned ledIndex  = (prog * SEGLEN * 3) >> 16;
    unsigned ledOffset = ledIndex;

    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 2));
    }

    if (ledIndex < SEGLEN) {  //wipe from 0 to 1
        for (unsigned i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, (i > ledOffset) ? SEGCOLOR(0) : SEGCOLOR(1));
        }
    } else if (ledIndex < SEGLEN * 2) {  //wipe from 1 to 2
        ledOffset = ledIndex - SEGLEN;
        for (unsigned i = ledOffset + 1; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGCOLOR(1));
        }
    } else  //wipe from 2 to 0
    {
        ledOffset = ledIndex - SEGLEN * 2;
        for (unsigned i = 0; i <= ledOffset; i++) {
            SEGMENT.setPixelColor(i, SEGCOLOR(0));
        }
    }

    return FRAMETIME;
}

// ---- Tri Fade -----------------------------------------------------------------
uint16_t mode_tricolor_fade(void) {
    uint16_t counter = strip.now * ((SEGMENT.speed >> 3) + 1);
    uint32_t prog    = (counter * 768) >> 16;

    uint32_t color1 = 0, color2 = 0;
    unsigned stage = 0;

    if (prog < 256) {
        color1 = SEGCOLOR(0);
        color2 = SEGCOLOR(1);
        stage  = 0;
    } else if (prog < 512) {
        color1 = SEGCOLOR(1);
        color2 = SEGCOLOR(2);
        stage  = 1;
    } else {
        color1 = SEGCOLOR(2);
        color2 = SEGCOLOR(0);
        stage  = 2;
    }

    byte stp = prog;  // % 256
    for (unsigned i = 0; i < SEGLEN; i++) {
        uint32_t color;
        if (stage == 2) {
            color = color_blend(SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 2), color2, stp);
        } else if (stage == 1) {
            color = color_blend(color1, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 2), stp);
        } else {
            color = color_blend(color1, color2, stp);
        }
        SEGMENT.setPixelColor(i, color);
    }

    return FRAMETIME;
}

// ---- Stream 2 -----------------------------------------------------------------
uint16_t mode_random_chase(void) {
    if (SEGENV.call == 0) {
        uint8_t r0  = random8();  // sequenced: WLED's RGBW32(random8(), random8(), random8(), 0) leaves the order open
        uint8_t g0  = random8();
        uint8_t b0  = random8();
        SEGENV.step = RGBW32(r0, g0, b0, 0);
        SEGENV.aux0 = random16();
    }
    unsigned prevSeed  = random16_get_seed();  // save seed so we can restore it at the end of the function
    uint32_t cycleTime = 25 + (3 * (uint32_t)(255 - SEGMENT.speed));
    uint32_t it        = strip.now / cycleTime;
    uint32_t color     = SEGENV.step;
    random16_set_seed(SEGENV.aux0);

    for (int i = SEGLEN - 1; i >= 0; i--) {
        uint8_t r = random8(6) != 0 ? (color >> 16 & 0xFF) : random8();
        uint8_t g = random8(6) != 0 ? (color >> 8 & 0xFF) : random8();
        uint8_t b = random8(6) != 0 ? (color & 0xFF) : random8();
        color     = RGBW32(r, g, b, 0);
        SEGMENT.setPixelColor(i, color);
        if (i == SEGLEN - 1U && SEGENV.aux1 != (it & 0xFFFFU)) {  //new first color in next frame
            SEGENV.step = color;
            SEGENV.aux0 = random16_get_seed();
        }
    }

    SEGENV.aux1 = it & 0xFFFF;

    random16_set_seed(prevSeed);  // restore original seed so other effects can use "random" PRNG
    return FRAMETIME;
}

// ---- Oscillate ----------------------------------------------------------------
//7 bytes (sizeof is 6: pos, size, dir, speed + 1 byte padding)
typedef struct Oscillator {
    uint16_t pos;
    uint8_t  size;
    int8_t   dir;
    uint8_t  speed;
} oscillator;

uint16_t mode_oscillate(void) {
    constexpr unsigned numOscillators = 3;
    constexpr unsigned dataSize       = sizeof(oscillator) * numOscillators;

    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    Oscillator* oscillators = reinterpret_cast<Oscillator*>(SEGENV.data);

    if (SEGENV.call == 0) {
        oscillators[0] = { (uint16_t)(SEGLEN / 4), (uint8_t)(SEGLEN / 8), 1, 1 };
        oscillators[1] = { (uint16_t)(SEGLEN / 4 * 3), (uint8_t)(SEGLEN / 8), 1, 2 };
        oscillators[2] = { (uint16_t)(SEGLEN / 4 * 2), (uint8_t)(SEGLEN / 8), -1, 1 };
    }

    uint32_t cycleTime = 20 + (2 * (uint32_t)(255 - SEGMENT.speed));
    uint32_t it        = strip.now / cycleTime;

    for (unsigned i = 0; i < numOscillators; i++) {
        // if the counter has increased, move the oscillator by the random step
        if (it != SEGENV.step) oscillators[i].pos += oscillators[i].dir * oscillators[i].speed;
        oscillators[i].size = SEGLEN / (3 + SEGMENT.intensity / 8);
        if ((oscillators[i].dir == -1) && (oscillators[i].pos > SEGLEN << 1)) {  // use integer overflow
            oscillators[i].pos = 0;
            oscillators[i].dir = 1;
            // make bigger steps for faster speeds
            oscillators[i].speed = SEGMENT.speed > 100 ? random8(2, 4) : random8(1, 3);
        }
        if ((oscillators[i].dir == 1) && (oscillators[i].pos >= (SEGLEN - 1))) {
            oscillators[i].pos   = SEGLEN - 1;
            oscillators[i].dir   = -1;
            oscillators[i].speed = SEGMENT.speed > 100 ? random8(2, 4) : random8(1, 3);
        }
    }

    for (unsigned i = 0; i < SEGLEN; i++) {
        uint32_t color = BLACK;
        for (unsigned j = 0; j < numOscillators; j++) {
            if ((int)i >= (int)oscillators[j].pos - oscillators[j].size && i <= oscillators[j].pos + oscillators[j].size) {
                color = (color == BLACK) ? SEGCOLOR(j) : color_blend(color, SEGCOLOR(j), uint8_t(128));
            }
        }
        SEGMENT.setPixelColor(i, color);
    }

    SEGENV.step = it;
    return FRAMETIME;
}

// ---- Rolling Balls ------------------------------------------------------------
// modified for balltrack mode
typedef struct RollingBall {
    uint32_t lastBounceUpdate;  // WLED: unsigned long, 32 bits on the ESP32 (made explicit for host builds)
    float    mass;              // could fix this to be = 1. if memory is an issue
    float    velocity;
    float    height;
} rball_t;

// Float `a + b * c` is split into two statements throughout: a compiler may
// otherwise fuse it into one FMA (clang does on arm64), which rounds
// differently from the app's float32 steps.
uint16_t rolling_balls(void) {
    //allocate segment data
    const unsigned maxNumBalls = 16;  // 255/16 + 1
    unsigned       dataSize    = sizeof(rball_t) * maxNumBalls;
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    rball_t* balls = reinterpret_cast<rball_t*>(SEGENV.data);

    // number of balls based on intensity setting to max of 16 (cycles colors)
    // non-chosen color is a random color
    unsigned numBalls = SEGMENT.intensity / 16 + 1;
    bool     hasCol2  = SEGCOLOR(2);

    if (SEGENV.call == 0) {
        SEGMENT.fill(hasCol2 ? BLACK : SEGCOLOR(1));  // start clean
        for (unsigned i = 0; i < maxNumBalls; i++) {
            balls[i].lastBounceUpdate = strip.now;
            balls[i].velocity         = 20.0f * float(random16(1000, 10000)) / 10000.0f;  // number from 1 to 10
            if (random8() < 128) balls[i].velocity = -balls[i].velocity;               // 50% chance of reverse direction
            balls[i].height = (float(random16(0, 10000)) / 10000.0f);                   // from 0. to 1.
            balls[i].mass   = (float(random16(1000, 10000)) / 10000.0f);                // from .1 to 1.
        }
    }

    float cfac = float(scale8(8, 255 - SEGMENT.speed) + 1) * 20000.0f;  // this uses the Aircoookie conversion factor for scaling time using speed slider

    if (SEGMENT.check3)
        SEGMENT.fade_out(250);  // 2-8 pixel trails (optional)
    else {
        if (!SEGMENT.check2) SEGMENT.fill(hasCol2 ? BLACK : SEGCOLOR(1));  // don't fill with background color if user wants to see trails
    }

    for (unsigned i = 0; i < numBalls; i++) {
        float timeSinceLastUpdate = float((strip.now - balls[i].lastBounceUpdate)) / cfac;
        float travel              = balls[i].velocity * timeSinceLastUpdate;
        float thisHeight          = balls[i].height + travel;  // this method keeps higher resolution
        // test if intensity level was increased and some balls are way off the track then put them back
        if (thisHeight < -0.5f || thisHeight > 1.5f) {
            thisHeight = balls[i].height = (float(random16(0, 10000)) / 10000.0f);  // from 0. to 1.
            balls[i].lastBounceUpdate    = strip.now;
        }
        // check if reached ends of the strip
        if ((thisHeight <= 0.0f && balls[i].velocity < 0.0f) || (thisHeight >= 1.0f && balls[i].velocity > 0.0f)) {
            balls[i].velocity         = -balls[i].velocity;  // reverse velocity
            balls[i].lastBounceUpdate = strip.now;
            balls[i].height           = thisHeight;
        }
        // check for collisions
        if (SEGMENT.check1) {
            for (unsigned j = i + 1; j < numBalls; j++) {
                if (balls[j].velocity != balls[i].velocity) {
                    //  tcollided + balls[j].lastBounceUpdate is acutal time of collision (this keeps precision with long to float conversions)
                    float tc1       = cfac * (balls[i].height - balls[j].height);
                    float tc2       = balls[i].velocity * float(balls[j].lastBounceUpdate - balls[i].lastBounceUpdate);
                    float tcollided = (tc1 + tc2) / (balls[j].velocity - balls[i].velocity);

                    if ((tcollided > 2.0f) && (tcollided < float(strip.now - balls[j].lastBounceUpdate))) {  // 2ms minimum to avoid duplicate bounces
                        balls[i].height = balls[i].height + balls[i].velocity * (tcollided + float(balls[j].lastBounceUpdate - balls[i].lastBounceUpdate)) / cfac;
                        balls[j].height = balls[i].height;
                        balls[i].lastBounceUpdate = (uint32_t)(tcollided + 0.5f) + balls[j].lastBounceUpdate;
                        balls[j].lastBounceUpdate = balls[i].lastBounceUpdate;
                        float vtmp                = balls[i].velocity;
                        float vi1                 = (balls[i].mass - balls[j].mass) * vtmp;
                        float vi2                 = 2.0f * balls[j].mass * balls[j].velocity;
                        balls[i].velocity         = (vi1 + vi2) / (balls[i].mass + balls[j].mass);
                        float vj1                 = (balls[j].mass - balls[i].mass) * balls[j].velocity;
                        float vj2                 = 2.0f * balls[i].mass * vtmp;
                        balls[j].velocity         = (vj1 + vj2) / (balls[i].mass + balls[j].mass);
                        thisHeight = balls[i].height + balls[i].velocity * (strip.now - balls[i].lastBounceUpdate) / cfac;
                    }
                }
            }
        }

        uint32_t color = SEGCOLOR(0);
        if (SEGMENT.palette) {
            //color = SEGMENT.color_wheel(i*(256/MAX(numBalls, 8)));
            color = SEGMENT.color_from_palette(i * 255 / numBalls, false, PALETTE_SOLID_WRAP, 0);
        } else if (hasCol2) {
            color = SEGCOLOR(i % NUM_COLORS);
        }

        if (thisHeight < 0.0f) thisHeight = 0.0f;
        if (thisHeight > 1.0f) thisHeight = 1.0f;
        unsigned pos = round(thisHeight * (SEGLEN - 1));
        SEGMENT.setPixelColor(pos, color);
        balls[i].lastBounceUpdate = strip.now;
        balls[i].height           = thisHeight;
    }

    return FRAMETIME;
}

}  // namespace wled
