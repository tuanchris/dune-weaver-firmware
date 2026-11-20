// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_e.ts; tools/fx_parity.py checks them.
//
// Batch e: Halloween Eyes, Fireworks Starburst, Fireworks 1D, Sinelon Dual,
// Sinelon Rainbow, Popcorn, Percent, Ripple Rainbow, Candle Multi, Solid Glitter.
#include "WledFx.h"

// WLED FX.h / FX.cpp constants the layer does not carry.  One segment here,
// and allocateData caps it at 4096 bytes: that whole cap is its fair share.
#ifndef FAIR_DATA_PER_SEG
#define FAIR_DATA_PER_SEG 4096
#endif
#ifndef FRAMETIME_FIXED
#define FRAMETIME_FIXED (1000 / 42)
#endif
#ifndef IBN
#define IBN 5100
#endif

namespace wled {

namespace {

//4 bytes
typedef struct Ripple {
    uint8_t  state;
    uint8_t  color;
    uint16_t pos;
} ripple;

//each needs 20 bytes
//Spark type is used for popcorn, 1D fireworks, and drip
typedef struct Spark {
    float    pos, posX;
    float    vel, velX;
    uint16_t col;
    uint8_t  colIndex;
} spark;

#define STARBURST_MAX_FRAG 10  //60 bytes / star
//each needs 20+STARBURST_MAX_FRAG*4 bytes
typedef struct particle {
    CRGB     color;
    uint32_t birth = 0;
    uint32_t last  = 0;
    float    vel   = 0;
    uint16_t pos   = -1;
    float    fragment[STARBURST_MAX_FRAG];
} star;

}  // namespace

// ---- Halloween Eyes -------------------------------------------------------------
// SEGMENT.offset (WLED reuses it on matrices): the layer's Segment has none.
static uint16_t eyes_offset = 0;

uint16_t mode_halloween_eyes(void) {
    enum eyeState : uint8_t {
        initializeOn = 0,
        on,
        blink,
        initializeOff,
        off,

        count
    };
    struct EyeData {
        eyeState state;
        uint8_t  color;
        uint16_t startPos;
        // duration + endTime could theoretically be replaced by a single endTime, however we would lose
        // the ability to end the animation early when the user reduces the animation time.
        uint16_t duration;
        uint32_t startTime;
        uint32_t blinkEndTime;
    };

    if (SEGLEN == 1) return mode_static();
    const unsigned maxWidth            = strip.isMatrix ? SEGMENT.virtualWidth() : SEGLEN;
    const unsigned HALLOWEEN_EYE_SPACE = MAX(2, strip.isMatrix ? SEGMENT.virtualWidth() >> 4 : SEGLEN >> 5);
    const unsigned HALLOWEEN_EYE_WIDTH = HALLOWEEN_EYE_SPACE / 2;
    unsigned       eyeLength           = (2 * HALLOWEEN_EYE_WIDTH) + HALLOWEEN_EYE_SPACE;
    if (eyeLength >= maxWidth) return mode_static();  //bail if segment too short

    if (!SEGENV.allocateData(sizeof(EyeData))) return mode_static();  //allocation failed
    EyeData& data = *reinterpret_cast<EyeData*>(SEGENV.data);

    if (!SEGMENT.check2) SEGMENT.fill(SEGCOLOR(1));  //fill background

    data.state                 = static_cast<eyeState>(data.state % eyeState::count);
    unsigned       duration    = max(uint16_t { 1u }, data.duration);
    const uint32_t elapsedTime = strip.now - data.startTime;

    switch (data.state) {
        case eyeState::initializeOn: {
            // initialize the eyes-on state:
            // - select eye position and color
            // - select a duration
            // - immediately switch to eyes on state.

            data.startPos = random16(0, maxWidth - eyeLength - 1);
            data.color    = random8();
            if (strip.isMatrix) eyes_offset = random16(SEGMENT.virtualHeight() - 1);  // a hack: reuse offset since it is not used in matrices
            duration      = 128u + random16(SEGMENT.intensity * 64u);
            data.duration = duration;
            data.state    = eyeState::on;
            [[fallthrough]];
        }
        case eyeState::on: {
            // eyes-on steate:
            // - fade eyes in for some time
            // - keep eyes on until the pre-selected duration is over
            // - randomly switch to the blink (sub-)state, and initialize it with a blink duration (more precisely, a blink end time stamp)
            // - never switch to the blink state if the animation just started or is about to end

            unsigned start2ndEye = data.startPos + HALLOWEEN_EYE_WIDTH + HALLOWEEN_EYE_SPACE;
            // If the user reduces the input while in this state, limit the duration.
            duration = min(duration, (128u + (SEGMENT.intensity * 64u)));

            constexpr uint32_t minimumOnTimeBegin   = 1024u;
            constexpr uint32_t minimumOnTimeEnd     = 1024u;
            const uint32_t     fadeInAnimationState = elapsedTime * uint32_t { 256u * 8u } / duration;
            const uint32_t     backgroundColor      = SEGCOLOR(1);
            const uint32_t     eyeColor             = SEGMENT.color_from_palette(data.color, false, false, 0);
            uint32_t           c                    = eyeColor;
            if (fadeInAnimationState < 256u) {
                c = color_blend(backgroundColor, eyeColor, fadeInAnimationState);
            } else if (elapsedTime > minimumOnTimeBegin) {
                const uint32_t remainingTime = (elapsedTime >= duration) ? 0u : (duration - elapsedTime);
                if (remainingTime > minimumOnTimeEnd) {
                    if (random8() < 4u) {
                        c                 = backgroundColor;
                        data.state        = eyeState::blink;
                        data.blinkEndTime = strip.now + random8(8, 128);
                    }
                }
            }

            if (c != backgroundColor) {
                // render eyes
                for (unsigned i = 0; i < HALLOWEEN_EYE_WIDTH; i++) {
                    if (strip.isMatrix) {
                        SEGMENT.setPixelColorXY(data.startPos + i, (unsigned)eyes_offset, c);
                        SEGMENT.setPixelColorXY(start2ndEye + i, (unsigned)eyes_offset, c);
                    } else {
                        SEGMENT.setPixelColor(data.startPos + i, c);
                        SEGMENT.setPixelColor(start2ndEye + i, c);
                    }
                }
            }
            break;
        }
        case eyeState::blink: {
            // eyes-on but currently blinking state:
            // - wait until the blink time is over, then switch back to eyes-on

            if (strip.now >= data.blinkEndTime) {
                data.state = eyeState::on;
            }
            break;
        }
        case eyeState::initializeOff: {
            // initialize eyes-off state:
            // - select a duration
            // - immediately switch to eyes-off state

            const unsigned eyeOffTimeBase = SEGMENT.speed * 128u;
            duration                      = eyeOffTimeBase + random16(eyeOffTimeBase);
            data.duration                 = duration;
            data.state                    = eyeState::off;
            [[fallthrough]];
        }
        case eyeState::off: {
            // eyes-off state:
            // - not much to do here

            // If the user reduces the input while in this state, limit the duration.
            const unsigned eyeOffTimeBase = SEGMENT.speed * 128u;
            duration                      = min(duration, (2u * eyeOffTimeBase));
            break;
        }
        case eyeState::count: {
            // Can't happen, not an actual state.
            data.state = eyeState::initializeOn;
            break;
        }
    }

    if (elapsedTime > duration) {
        // The current state duration is over, switch to the next state.
        switch (data.state) {
            case eyeState::initializeOn:
            case eyeState::on:
            case eyeState::blink:
                data.state = eyeState::initializeOff;
                break;
            case eyeState::initializeOff:
            case eyeState::off:
            case eyeState::count:
            default:
                data.state = eyeState::initializeOn;
                break;
        }
        data.startTime = strip.now;
    }

    return FRAMETIME;
}

// ---- Fireworks Starburst ----------------------------------------------------------
/*
/ Fireworks in starburst effect
/ based on the video: https://www.reddit.com/r/arduino/comments/c3sd46/i_made_this_fireworks_effect_for_my_led_strips/
/ Speed sets frequency of new starbursts, intensity is the intensity of the burst
*/
uint16_t mode_starburst(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned maxData = FAIR_DATA_PER_SEG;  //ESP8266: 256 ESP32: 640
    unsigned segs    = strip.getActiveSegmentsNum();
    if (segs <= (strip.getMaxSegments() / 2)) maxData *= 2;  //ESP8266: 512 if <= 8 segs ESP32: 1280 if <= 16 segs
    if (segs <= (strip.getMaxSegments() / 4)) maxData *= 2;  //ESP8266: 1024 if <= 4 segs ESP32: 2560 if <= 8 segs
    unsigned maxStars = maxData / sizeof(star);              //ESP8266: max. 4/9/19 stars/seg, ESP32: max. 10/21/42 stars/seg

    unsigned numStars = 1 + (SEGLEN >> 3);
    if (numStars > maxStars) numStars = maxStars;
    unsigned dataSize = sizeof(star) * numStars;

    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    uint32_t it = strip.now;

    star* stars = reinterpret_cast<star*>(SEGENV.data);

    float maxSpeed         = 375.0f;   // Max velocity
    float particleIgnition = 250.0f;   // How long to "flash"
    float particleFadeTime = 1500.0f;  // Fade out time

    for (unsigned j = 0; j < numStars; j++) {
        // speed to adjust chance of a burst, max is nearly always.
        if (random8((144 - (SEGMENT.speed >> 1))) == 0 && stars[j].birth == 0) {
            // Pick a random color and location.
            unsigned startPos   = random16(SEGLEN - 1);
            float    multiplier = (float)(random8()) / 255.0f * 1.0f;

            stars[j].color = CRGB(SEGMENT.color_wheel(random8()));
            stars[j].pos   = startPos;
            stars[j].vel   = maxSpeed * (float)(random8()) / 255.0f * multiplier;
            stars[j].birth = it;
            stars[j].last  = it;
            // more fragments means larger burst effect
            int num = random8(3, 6 + (SEGMENT.intensity >> 5));

            for (int i = 0; i < STARBURST_MAX_FRAG; i++) {
                if (i < num)
                    stars[j].fragment[i] = startPos;
                else
                    stars[j].fragment[i] = -1;
            }
        }
    }

    if (!SEGMENT.check2) SEGMENT.fill(SEGCOLOR(1));

    for (unsigned j = 0; j < numStars; j++) {
        if (stars[j].birth != 0) {
            float dt = (it - stars[j].last) / 1000.0;

            for (int i = 0; i < STARBURST_MAX_FRAG; i++) {
                int var = i >> 1;

                if (stars[j].fragment[i] > 0) {
                    //all fragments travel right, will be mirrored on other side
                    stars[j].fragment[i] += stars[j].vel * dt * (float)var / 3.0;
                }
            }
            stars[j].last = it;
            stars[j].vel -= 3 * stars[j].vel * dt;
        }

        CRGB c = stars[j].color;

        // If the star is brand new, it flashes white briefly.
        // Otherwise it just fades over time.
        float fade = 0.0f;
        float age  = it - stars[j].birth;

        if (age < particleIgnition) {
            c = CRGB(color_blend(WHITE, RGBW32(c.r, c.g, c.b, 0), 254.5f * ((age / particleIgnition))));
        } else {
            // Figure out how much to fade and shrink the star based on
            // its age relative to its lifetime
            if (age > particleIgnition + particleFadeTime) {
                fade           = 1.0f;  // Black hole, all faded out
                stars[j].birth = 0;
                c              = CRGB(SEGCOLOR(1));
            } else {
                age -= particleIgnition;
                fade   = (age / particleFadeTime);  // Fading star
                byte f = 254.5f * fade;
                c      = CRGB(color_blend(RGBW32(c.r, c.g, c.b, 0), SEGCOLOR(1), f));
            }
        }

        float particleSize = (1.0f - fade) * 2.0f;

        for (size_t index = 0; index < STARBURST_MAX_FRAG * 2; index++) {
            bool     mirrored = index & 0x1;
            unsigned i        = index >> 1;
            if (stars[j].fragment[i] > 0) {
                float loc = stars[j].fragment[i];
                if (mirrored) loc -= (loc - stars[j].pos) * 2;
                int start = loc - particleSize;
                int end   = loc + particleSize;
                if (start < 0) start = 0;
                if (start == end) end++;
                if (end > SEGLEN) end = SEGLEN;
                for (int p = start; p < end; p++) {
                    SEGMENT.setPixelColor(p, c.r, c.g, c.b);
                }
            }
        }
    }
    return FRAMETIME;
}
#undef STARBURST_MAX_FRAG

// ---- Fireworks 1D -----------------------------------------------------------------
/*
 * Exploding fireworks effect
 * adapted from: http://www.anirama.com/1000leds/1d-fireworks/
 * adapted for 2D WLED by blazoncek (Blaz Kristan (AKA blazoncek))
 */
uint16_t mode_exploding_fireworks(void) {
    if (SEGLEN == 1) return mode_static();
    const int cols = SEGMENT.is2D() ? SEGMENT.virtualWidth() : 1;
    const int rows = SEGMENT.is2D() ? SEGMENT.virtualHeight() : SEGMENT.virtualLength();

    //allocate segment data
    unsigned maxData = FAIR_DATA_PER_SEG;  //ESP8266: 256 ESP32: 640
    unsigned segs    = strip.getActiveSegmentsNum();
    if (segs <= (strip.getMaxSegments() / 2)) maxData *= 2;  //ESP8266: 512 if <= 8 segs ESP32: 1280 if <= 16 segs
    if (segs <= (strip.getMaxSegments() / 4)) maxData *= 2;  //ESP8266: 1024 if <= 4 segs ESP32: 2560 if <= 8 segs
    int maxSparks = maxData / sizeof(spark);                 //ESP8266: max. 21/42/85 sparks/seg, ESP32: max. 53/106/213 sparks/seg

    unsigned numSparks = min(5 + ((rows * cols) >> 1), maxSparks);
    unsigned dataSize  = sizeof(spark) * numSparks;
    if (!SEGENV.allocateData(dataSize + sizeof(float))) return mode_static();  //allocation failed
    float* dying_gravity = reinterpret_cast<float*>(SEGENV.data + dataSize);

    if (dataSize != SEGENV.aux1) {  //reset to flare if sparks were reallocated (it may be good idea to reset segment if bounds change)
        *dying_gravity = 0.0f;
        SEGENV.aux0    = 0;
        SEGENV.aux1    = dataSize;
    }

    SEGMENT.fade_out(252);

    Spark* sparks = reinterpret_cast<Spark*>(SEGENV.data);
    Spark* flare  = sparks;  //first spark is flare data

    float gravity = -0.0004f - (SEGMENT.speed / 800000.0f);  // m/s/s
    gravity *= rows;

    if (SEGENV.aux0 < 2) {        //FLARE
        if (SEGENV.aux0 == 0) {  //init flare
            flare->pos            = 0;
            flare->posX           = SEGMENT.is2D() ? random16(2, cols - 3) : (SEGMENT.intensity > random8());  // will enable random firing side on 1D
            unsigned peakHeight   = 75 + random8(180);                                                        //0-255
            peakHeight            = (peakHeight * (rows - 1)) >> 8;
            flare->vel            = sqrtf(-2.0f * gravity * peakHeight);
            flare->velX           = SEGMENT.is2D() ? (random8(9) - 4) / 64.0f : 0;  // no X velocity on 1D
            flare->col            = 255;                                            //brightness
            SEGENV.aux0           = 1;
        }

        // launch
        if (flare->vel > 12 * gravity) {
            // flare
            if (SEGMENT.is2D())
                SEGMENT.setPixelColorXY(unsigned(flare->posX), rows - uint16_t(flare->pos) - 1, flare->col, flare->col, flare->col);
            else
                SEGMENT.setPixelColor((flare->posX > 0.0f) ? rows - int(flare->pos) - 1 : int(flare->pos), flare->col, flare->col, flare->col);
            flare->pos += flare->vel;
            flare->pos = constrain(flare->pos, 0, rows - 1);
            if (SEGMENT.is2D()) {
                flare->posX += flare->velX;
                flare->posX = constrain(flare->posX, 0, cols - 1);
            }
            flare->vel += gravity;
            flare->col -= 2;
        } else {
            SEGENV.aux0 = 2;  // ready to explode
        }
    } else if (SEGENV.aux0 < 4) {
        /*
         * Explode!
         *
         * Explosion happens where the flare ended.
         * Size is proportional to the height.
         */
        unsigned nSparks = flare->pos + random8(4);
        nSparks          = std::max(nSparks, 4U);  // This is not a standard constrain; numSparks is not guaranteed to be at least 4
        nSparks          = std::min(nSparks, numSparks);

        // initialize sparks
        if (SEGENV.aux0 == 2) {
            for (unsigned i = 1; i < nSparks; i++) {
                sparks[i].pos  = flare->pos;
                sparks[i].posX = flare->posX;
                sparks[i].vel  = (float(random16(20001)) / 10000.0f) - 0.9f;  // from -0.9 to 1.1
                sparks[i].vel *= rows < 32 ? 0.5f : 1;                           // reduce velocity for smaller strips
                sparks[i].velX     = SEGMENT.is2D() ? (float(random16(20001)) / 10000.0f) - 1.0f : 0;  // from -1 to 1
                sparks[i].col      = 345;  //abs(sparks[i].vel * 750.0); // set colors before scaling velocity to keep them bright
                //sparks[i].col = constrain(sparks[i].col, 0, 345);
                sparks[i].colIndex = random8();
                sparks[i].vel *= flare->pos / rows;                         // proportional to height
                sparks[i].velX *= SEGMENT.is2D() ? flare->posX / cols : 0;  // proportional to width
                sparks[i].vel *= -gravity * 50;
            }
            //sparks[1].col = 345; // this will be our known spark
            *dying_gravity = gravity / 2;
            SEGENV.aux0    = 3;
        }

        if (sparks[1].col > 4) {  //&& sparks[1].pos > 0) { // as long as our known spark is lit, work with all the sparks
            for (unsigned i = 1; i < nSparks; i++) {
                sparks[i].pos += sparks[i].vel;
                sparks[i].posX += sparks[i].velX;
                sparks[i].vel += *dying_gravity;
                sparks[i].velX += SEGMENT.is2D() ? *dying_gravity : 0;
                if (sparks[i].col > 3) sparks[i].col -= 4;

                if (sparks[i].pos > 0 && sparks[i].pos < rows) {
                    if (SEGMENT.is2D() && !(sparks[i].posX >= 0 && sparks[i].posX < cols)) continue;
                    unsigned prog    = sparks[i].col;
                    uint32_t spColor = (SEGMENT.palette) ? SEGMENT.color_wheel(sparks[i].colIndex) : SEGCOLOR(0);
                    CRGB     c       = CRGB::Black;  //HeatColor(sparks[i].col);
                    if (prog > 300) {                //fade from white to spark color
                        c = CRGB(color_blend(spColor, WHITE, (prog - 300) * 5));
                    } else if (prog > 45) {  //fade from spark color to black
                        c                = CRGB(color_blend(BLACK, spColor, prog - 45));
                        unsigned cooling = (300 - prog) >> 5;
                        c.g              = qsub8(c.g, cooling);
                        c.b              = qsub8(c.b, cooling * 2);
                    }
                    if (SEGMENT.is2D())
                        SEGMENT.setPixelColorXY(int(sparks[i].posX), rows - int(sparks[i].pos) - 1, c.red, c.green, c.blue);
                    else
                        SEGMENT.setPixelColor(int(sparks[i].posX) ? rows - int(sparks[i].pos) - 1 : int(sparks[i].pos), c.red, c.green, c.blue);
                }
            }
            if (SEGMENT.check3) SEGMENT.blur(16);
            *dying_gravity *= .8f;  // as sparks burn out they fall slower
        } else {
            SEGENV.aux0 = 6 + random8(10);  //wait for this many frames
        }
    } else {
        SEGENV.aux0--;
        if (SEGENV.aux0 < 4) {
            SEGENV.aux0 = 0;  //back to flare
        }
    }

    return FRAMETIME;
}

// ---- Sinelon Dual / Sinelon Rainbow ---------------------------------------------------
/*
* Sinelon stolen from FASTLED examples
*/
static uint16_t sinelon_base(bool dual, bool rainbow = false) {
    if (SEGLEN == 1) return mode_static();
    SEGMENT.fade_out(SEGMENT.intensity);
    unsigned pos = beatsin16_t(SEGMENT.speed / 10, 0, SEGLEN - 1);
    if (SEGENV.call == 0) SEGENV.aux0 = pos;
    uint32_t color1 = SEGMENT.color_from_palette(pos, true, false, 0);
    uint32_t color2 = SEGCOLOR(2);
    if (rainbow) {
        color1 = SEGMENT.color_wheel((pos & 0x07) * 32);
    }
    SEGMENT.setPixelColor(pos, color1);
    if (dual) {
        if (!color2) color2 = SEGMENT.color_from_palette(pos, true, false, 0);
        if (rainbow) color2 = color1;  //rainbow
        SEGMENT.setPixelColor(SEGLEN - 1 - pos, color2);
    }
    if (SEGENV.aux0 != pos) {
        if (SEGENV.aux0 < pos) {
            for (unsigned i = SEGENV.aux0; i < pos; i++) {
                SEGMENT.setPixelColor(i, color1);
                if (dual) SEGMENT.setPixelColor(SEGLEN - 1 - i, color2);
            }
        } else {
            for (unsigned i = SEGENV.aux0; i > pos; i--) {
                SEGMENT.setPixelColor(i, color1);
                if (dual) SEGMENT.setPixelColor(SEGLEN - 1 - i, color2);
            }
        }
        SEGENV.aux0 = pos;
    }

    return FRAMETIME;
}

uint16_t mode_sinelon_dual(void) {
    return sinelon_base(true);
}

uint16_t mode_sinelon_rainbow(void) {
    return sinelon_base(false, true);
}

// ---- Solid Glitter ------------------------------------------------------------------
// utility function that will add random glitter to SEGMENT
static void glitter_base(uint8_t intensity, uint32_t col = ULTRAWHITE) {
    if (intensity > random8()) SEGMENT.setPixelColor(random16(SEGLEN), col);
}

//Solid colour background with glitter (can be replaced by Glitter)
uint16_t mode_solid_glitter(void) {
    SEGMENT.fill(SEGCOLOR(0));
    glitter_base(SEGMENT.intensity, SEGCOLOR(2) ? SEGCOLOR(2) : ULTRAWHITE);
    return FRAMETIME;
}

// ---- Popcorn ------------------------------------------------------------------------
#define maxNumPopcorn 21  // max 21 on 16 segment ESP8266
/*
*  POPCORN
*  modified from https://github.com/kitesurfer1404/WS2812FX/blob/master/src/custom/Popcorn.h
*/
uint16_t mode_popcorn(void) {
    if (SEGLEN == 1) return mode_static();
    //allocate segment data
    unsigned strips         = SEGMENT.nrOfVStrips();
    unsigned usablePopcorns = maxNumPopcorn;
    if (usablePopcorns * strips * sizeof(spark) > FAIR_DATA_PER_SEG)
        usablePopcorns = FAIR_DATA_PER_SEG / (strips * sizeof(spark)) + 1;  // at least 1 popcorn per vstrip
    unsigned dataSize = sizeof(spark) * usablePopcorns;  // on a matrix 64x64 this could consume a little less than 27kB when Bar expansion is used
    if (!SEGENV.allocateData(dataSize * strips)) return mode_static();  //allocation failed

    Spark* popcorn = reinterpret_cast<Spark*>(SEGENV.data);

    bool hasCol2 = SEGCOLOR(2);
    if (!SEGMENT.check2) SEGMENT.fill(hasCol2 ? BLACK : SEGCOLOR(1));

    struct virtualStrip {
        static void runStrip(uint16_t stripNr, Spark* popcorn, unsigned usablePopcorns) {
            float gravity = -0.0001f - (SEGMENT.speed / 200000.0f);  // m/s/s
            gravity *= SEGLEN;

            unsigned numPopcorn = SEGMENT.intensity * usablePopcorns / 255;
            if (numPopcorn == 0) numPopcorn = 1;

            for (unsigned i = 0; i < numPopcorn; i++) {
                if (popcorn[i].pos >= 0.0f) {  // if kernel is active, update its position
                    popcorn[i].pos += popcorn[i].vel;
                    popcorn[i].vel += gravity;
                } else {                  // if kernel is inactive, randomly pop it
                    if (random8() < 2) {  // POP!!!
                        popcorn[i].pos = 0.01f;

                        unsigned peakHeight = 128 + random8(128);  //0-255
                        peakHeight          = (peakHeight * (SEGLEN - 1)) >> 8;
                        popcorn[i].vel      = sqrtf(-2.0f * gravity * peakHeight);

                        if (SEGMENT.palette) {
                            popcorn[i].colIndex = random8();
                        } else {
                            byte col = random8(0, NUM_COLORS);
                            if (!SEGCOLOR(2) || !SEGCOLOR(col)) col = 0;
                            popcorn[i].colIndex = col;
                        }
                    }
                }
                if (popcorn[i].pos >= 0.0f) {  // draw now active popcorn (either active before or just popped)
                    uint32_t col = SEGMENT.color_wheel(popcorn[i].colIndex);
                    if (!SEGMENT.palette && popcorn[i].colIndex < NUM_COLORS) col = SEGCOLOR(popcorn[i].colIndex);
                    unsigned ledIndex = popcorn[i].pos;
                    if (ledIndex < SEGLEN) SEGMENT.setPixelColor(indexToVStrip(ledIndex, stripNr), col);
                }
            }
        }
    };

    for (unsigned stripNr = 0; stripNr < strips; stripNr++)
        virtualStrip::runStrip(stripNr, &popcorn[stripNr * usablePopcorns], usablePopcorns);

    return FRAMETIME;
}
#undef maxNumPopcorn

// ---- Percent ------------------------------------------------------------------------
/*
 * Percentage display
 * Intensity values from 0-100 turn on the leds.
 */
uint16_t mode_percent(void) {
    unsigned percent     = SEGMENT.intensity;
    percent              = constrain(percent, 0, 200);
    unsigned active_leds = (percent < 100) ? roundf(SEGLEN * percent / 100.0f) : roundf(SEGLEN * (200 - percent) / 100.0f);

    unsigned size = (1 + ((SEGMENT.speed * SEGLEN) >> 11));
    if (SEGMENT.speed == 255) size = 255;

    if (percent <= 100) {
        for (int i = 0; i < SEGLEN; i++) {
            if (i < SEGENV.aux1) {
                if (SEGMENT.check1)
                    SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(map(percent, 0, 100, 0, 255), false, false, 0));
                else
                    SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
            } else {
                SEGMENT.setPixelColor(i, SEGCOLOR(1));
            }
        }
    } else {
        for (int i = 0; i < SEGLEN; i++) {
            if (i < (SEGLEN - SEGENV.aux1)) {
                SEGMENT.setPixelColor(i, SEGCOLOR(1));
            } else {
                if (SEGMENT.check1)
                    SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(map(percent, 100, 200, 255, 0), false, false, 0));
                else
                    SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
            }
        }
    }

    if (active_leds > SEGENV.aux1) {  // smooth transition to the target value
        SEGENV.aux1 += size;
        if (SEGENV.aux1 > active_leds) SEGENV.aux1 = active_leds;
    } else if (active_leds < SEGENV.aux1) {
        if (SEGENV.aux1 > size)
            SEGENV.aux1 -= size;
        else
            SEGENV.aux1 = 0;
        if (SEGENV.aux1 < active_leds) SEGENV.aux1 = active_leds;
    }

    return FRAMETIME;
}

// ---- Ripple Rainbow -------------------------------------------------------------------
//Water ripple
//propagation velocity from speed
//drop rate from intensity
#define MAX_RIPPLES 100
static uint16_t ripple_base() {
    unsigned maxRipples = min(1 + (SEGLEN >> 2), MAX_RIPPLES);  // 56 max for 16 segment ESP8266
    unsigned dataSize   = sizeof(ripple) * maxRipples;

    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    Ripple* ripples = reinterpret_cast<Ripple*>(SEGENV.data);

    //draw wave
    for (unsigned i = 0; i < maxRipples; i++) {
        unsigned ripplestate = ripples[i].state;
        if (ripplestate) {
            unsigned rippledecay  = (SEGMENT.speed >> 4) + 1;  //faster decay if faster propagation
            unsigned rippleorigin = ripples[i].pos;
            uint32_t col          = SEGMENT.color_from_palette(ripples[i].color, false, false, 255);
            unsigned propagation  = ((ripplestate / rippledecay - 1) * (SEGMENT.speed + 1));
            int      propI        = propagation >> 8;
            unsigned propF        = propagation & 0xFF;
            unsigned amp          = (ripplestate < 17) ? triwave8((ripplestate - 1) * 8) : map(ripplestate, 17, 255, 255, 2);

            if (SEGMENT.is2D()) {
                propI /= 2;
                unsigned cx  = rippleorigin >> 8;
                unsigned cy  = rippleorigin & 0xFF;
                unsigned mag = scale8(sin8_t((propF >> 2)), amp);
                if (propI > 0) SEGMENT.drawCircle(cx, cy, propI, color_blend(SEGMENT.getPixelColorXY(cx + propI, cy), col, mag), true);
            } else {
                int left  = rippleorigin - propI - 1;
                int right = rippleorigin + propI + 2;
                for (int v = 0; v < 4; v++) {
                    unsigned mag = scale8(cubicwave8((propF >> 2) + v * 64), amp);
                    SEGMENT.setPixelColor(left + v, color_blend(SEGMENT.getPixelColor(left + v), col, mag));     // TODO
                    SEGMENT.setPixelColor(right - v, color_blend(SEGMENT.getPixelColor(right - v), col, mag));  // TODO
                }
            }
            ripplestate += rippledecay;
            ripples[i].state = (ripplestate > 254) ? 0 : ripplestate;
        } else {  //randomly create new wave
            if (random16(IBN + 10000) <= (SEGMENT.intensity >> (SEGMENT.is2D() * 3))) {
                ripples[i].state = 1;
                if (SEGMENT.is2D()) {
                    unsigned rx    = random8(SEGENV.virtualWidth());  // sequenced: `a<<8 | b` leaves the RNG order open
                    ripples[i].pos = (rx << 8) | (random8(SEGENV.virtualHeight()));
                } else {
                    ripples[i].pos = random16(SEGLEN);
                }
                ripples[i].color = random8();  //color
            }
        }
    }

    return FRAMETIME;
}
#undef MAX_RIPPLES

uint16_t mode_ripple_rainbow(void) {
    if (SEGLEN == 1) return mode_static();
    if (SEGENV.call == 0) {
        SEGENV.aux0 = random8();
        SEGENV.aux1 = random8();
    }
    if (SEGENV.aux0 == SEGENV.aux1) {
        SEGENV.aux1 = random8();
    } else if (SEGENV.aux1 > SEGENV.aux0) {
        SEGENV.aux0++;
    } else {
        SEGENV.aux0--;
    }
    SEGMENT.fill(color_blend(SEGMENT.color_wheel(SEGENV.aux0), BLACK, 235));
    return ripple_base();
}

// ---- Candle Multi -------------------------------------------------------------------
//values close to 100 produce 5Hz flicker, which looks very candle-y
//Inspired by https://github.com/avanhanegem/ArduinoCandleEffectNeoPixel
//and https://cpldcpu.wordpress.com/2016/01/05/reverse-engineering-a-real-candle/
static uint16_t candle(bool multi) {
    if (multi && SEGLEN > 1) {
        //allocate segment data
        unsigned dataSize = max(1, SEGLEN - 1) * 3;                    //max. 1365 pixels (ESP8266)
        if (!SEGENV.allocateData(dataSize)) return candle(false);  //allocation failed
    }

    //max. flicker range controlled by intensity
    unsigned valrange = SEGMENT.intensity;
    unsigned rndval   = valrange >> 1;  //max 127

    //step (how much to move closer to target per frame) coarsely set by speed
    unsigned speedFactor = 4;
    if (SEGMENT.speed > 252) {  //epilepsy
        speedFactor = 1;
    } else if (SEGMENT.speed > 99) {  //regular candle (mode called every ~25 ms, so 4 frames to have a new target every 100ms)
        speedFactor = 2;
    } else if (SEGMENT.speed > 49) {  //slower fade
        speedFactor = 3;
    }  //else 4 (slowest)

    unsigned numCandles = (multi) ? SEGLEN : 1;

    for (unsigned i = 0; i < numCandles; i++) {
        unsigned d = 0;  //data location

        unsigned s = SEGENV.aux0, s_target = SEGENV.aux1, fadeStep = SEGENV.step;
        if (i > 0) {
            d        = (i - 1) * 3;
            s        = SEGENV.data[d];
            s_target = SEGENV.data[d + 1];
            fadeStep = SEGENV.data[d + 2];
        }
        if (fadeStep == 0) {  //init vals
            s        = 128;
            s_target = 130 + random8(4);
            fadeStep = 1;
        }

        bool newTarget = false;
        if (s_target > s) {  //fade up
            s = qadd8(s, fadeStep);
            if (s >= s_target) newTarget = true;
        } else {
            s = qsub8(s, fadeStep);
            if (s <= s_target) newTarget = true;
        }

        if (newTarget) {
            unsigned r1 = random8(rndval);  // sequenced (the sum is order-independent anyway)
            s_target    = r1 + random8(rndval);  //between 0 and rndval*2 -2 = 252
            if (s_target < (rndval >> 1)) s_target = (rndval >> 1) + random8(rndval);
            unsigned offset = (255 - valrange);
            s_target += offset;

            unsigned dif = (s_target > s) ? s_target - s : s - s_target;

            fadeStep = dif >> speedFactor;
            if (fadeStep == 0) fadeStep = 1;
        }

        if (i > 0) {
            SEGMENT.setPixelColor(i, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0), s));

            SEGENV.data[d]     = s;
            SEGENV.data[d + 1] = s_target;
            SEGENV.data[d + 2] = fadeStep;
        } else {
            for (int j = 0; j < SEGLEN; j++) {
                SEGMENT.setPixelColor(j, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(j, true, PALETTE_SOLID_WRAP, 0), s));
            }

            SEGENV.aux0 = s;
            SEGENV.aux1 = s_target;
            SEGENV.step = fadeStep;
        }
    }

    return FRAMETIME_FIXED;
}

uint16_t mode_candle_multi(void) {
    return candle(true);
}

}  // namespace wled
