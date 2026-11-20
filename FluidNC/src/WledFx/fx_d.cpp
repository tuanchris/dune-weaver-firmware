// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_d.ts; tools/fx_parity.py checks them.
//
// Batch d: Palette, Fill Noise, Noise 2-4, Colortwinkles, Lake, Meteor Smooth,
// Twinklefox, Twinklecat, Solid Pattern (+ Tri), Spots, Spots Fade.
#include "WledFx.h"

// WLED's fixed frame time (FX.h: 1000 / WLED_FPS, 42 fps).  Not the layer's
// 33 ms FRAMETIME: a 33 ms return makes the runner skip every other frame.
#ifndef FRAMETIME_FIXED
#    define FRAMETIME_FIXED (1000 / 42)
#endif
#ifndef M_TWOPI
#    define M_TWOPI (M_PI * 2.0)
#endif

namespace wled {

// ---- Palette ------------------------------------------------------------------
uint16_t mode_palette(void) {
    // Set up some compile time constants so that we can handle integer and float based modes using the same code base.
    using mathType     = float;
    using wideMathType = float;
    using angleType    = float;
    constexpr mathType sInt16Scale           = 1.0f;
    constexpr mathType maxAngle              = M_PI / 256.0;
    constexpr mathType staticRotationScale   = 1.0f;
    constexpr mathType animatedRotationScale = M_TWOPI / double(0xFFFF);
    constexpr float (*sinFunction)(float)    = &sin_t;
    constexpr float (*cosFunction)(float)    = &cos_t;

    const bool isMatrix = strip.isMatrix;
    const int  cols     = SEGMENT.virtualWidth();
    const int  rows     = isMatrix ? SEGMENT.virtualHeight() : strip.getActiveSegmentsNum();

    const int  inputShift           = SEGMENT.speed;
    const int  inputSize            = SEGMENT.intensity;
    const int  inputRotation        = SEGMENT.custom1;
    const bool inputAnimateShift    = SEGMENT.check1;
    const bool inputAnimateRotation = SEGMENT.check2;
    const bool inputAssumeSquare    = SEGMENT.check3;

    const angleType theta    = (!inputAnimateRotation) ? ((inputRotation + 128) * maxAngle / staticRotationScale)
                                                       : (((strip.now * ((inputRotation >> 4) + 1)) & 0xFFFF) * animatedRotationScale);
    const mathType  sinTheta = sinFunction(theta);
    const mathType  cosTheta = cosFunction(theta);

    const mathType maxX = std::max(1, cols - 1);
    const mathType maxY = std::max(1, rows - 1);
    // Set up some parameters according to inputAssumeSquare, so that we can handle anamorphic mode using the same code base.
    const mathType maxXIn  = inputAssumeSquare ? maxX : mathType(1);
    const mathType maxYIn  = inputAssumeSquare ? maxY : mathType(1);
    const mathType maxXOut = !inputAssumeSquare ? maxX : mathType(1);
    const mathType maxYOut = !inputAssumeSquare ? maxY : mathType(1);
    const mathType centerX = sInt16Scale * maxXOut / mathType(2);
    const mathType centerY = sInt16Scale * maxYOut / mathType(2);
    // The rectangle is always scaled so that its left and right most points match the display's sides.
    const mathType scale = std::abs(sinTheta) + (std::abs(cosTheta) * maxYOut / maxXOut);
    // 2D simulation: a 1D setup treats each segment as one line of a 2-dimensional display.
    const int yFrom = isMatrix ? 0 : strip.getCurrSegmentId();
    const int yTo   = isMatrix ? maxY : yFrom;
    for (int y = yFrom; y <= yTo; ++y) {
        // translate, scale, rotate
        const mathType ytCosTheta =
            mathType((wideMathType(cosTheta) * wideMathType(y * sInt16Scale - centerY * maxYIn)) / wideMathType(maxYIn * scale));
        for (int x = 0; x < cols; ++x) {
            // translate, scale, rotate
            const mathType xtSinTheta =
                mathType((wideMathType(sinTheta) * wideMathType(x * sInt16Scale - centerX * maxXIn)) / wideMathType(maxXIn * scale));
            // Map the pixel coordinate to an imaginary-rectangle-coordinate.
            const mathType sourceX = xtSinTheta + ytCosTheta + centerX;
            // Clamp to [0, maxXOut] for imprecision, then scale to [0, 255] for the palette.
            int colorIndex = (std::min(std::max(sourceX, mathType(0)), maxXOut * sInt16Scale) * wideMathType(255)) / (sInt16Scale * maxXOut);
            // inputSize: values < 128 display a fraction of a palette, values > 128 display multiple palettes.
            if (inputSize <= 128) {
                colorIndex = (colorIndex * inputSize) / 128;
            } else {
                // Linear function that maps colorIndex 128=>1, 256=>9.
                colorIndex = ((inputSize - 112) * colorIndex) / 16;
            }
            // Finally, shift the palette a bit.
            const int paletteOffset = (!inputAnimateShift) ? (inputShift) : (((strip.now * ((inputShift >> 3) + 1)) & 0xFFFF) >> 8);
            colorIndex -= paletteOffset;
            const uint32_t color = SEGMENT.color_wheel((uint8_t)colorIndex);
            if (isMatrix) {
                SEGMENT.setPixelColorXY(x, y, color);
            } else {
                SEGMENT.setPixelColor(x, color);
            }
        }
    }
    return FRAMETIME;
}

// ---- Fill Noise -------------------------------------------------------------------
uint16_t mode_fillnoise8(void) {
    if (SEGENV.call == 0) SEGENV.step = random16(12345);
    for (int i = 0; i < SEGLEN; i++) {
        unsigned index = inoise8(i * SEGLEN, SEGENV.step + i * SEGLEN);
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0));
    }
    SEGENV.step += beatsin8_t(SEGMENT.speed, 1, 6);  //10,1,4

    return FRAMETIME;
}

// ---- Noise 2 ----------------------------------------------------------------------
uint16_t mode_noise16_2(void) {
    unsigned scale = 1000;  // the "zoom factor" for the noise
    SEGENV.step += (1 + (SEGMENT.speed >> 1));

    for (int i = 0; i < SEGLEN; i++) {
        unsigned shift_x = SEGENV.step >> 6;                 // x as a function of time
        uint32_t real_x  = (i + shift_x) * scale;            // calculate the coordinates within the noise field
        unsigned noise   = inoise16(real_x, 0, 4223) >> 8;   // get the noise data and scale it down
        unsigned index   = sin8_t(noise * 3);                // map led color based on noise data

        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0, noise));
    }

    return FRAMETIME;
}

// ---- Noise 3 ----------------------------------------------------------------------
uint16_t mode_noise16_3(void) {
    unsigned scale = 800;  // the "zoom factor" for the noise
    SEGENV.step += (1 + SEGMENT.speed);

    for (int i = 0; i < SEGLEN; i++) {
        unsigned shift_x = 4223;  // no movement along x and y
        unsigned shift_y = 1234;
        uint32_t real_x  = (i + shift_x) * scale;  // calculate the coordinates within the noise field
        uint32_t real_y  = (i + shift_y) * scale;  // based on the precalculated positions
        uint32_t real_z  = SEGENV.step * 8;
        unsigned noise   = inoise16(real_x, real_y, real_z) >> 8;  // get the noise data and scale it down
        unsigned index   = sin8_t(noise * 3);                      // map led color based on noise data

        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0, noise));
    }

    return FRAMETIME;
}

// ---- Noise 4 ----------------------------------------------------------------------
//https://github.com/aykevl/ledstrip-spark/blob/master/ledstrip.ino
uint16_t mode_noise16_4(void) {
    uint32_t stp = (strip.now * SEGMENT.speed) >> 7;
    for (int i = 0; i < SEGLEN; i++) {
        int index = inoise16(uint32_t(i) << 12, stp);
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0));
    }
    return FRAMETIME;
}

// ---- Colortwinkles ----------------------------------------------------------------
//based on https://gist.github.com/kriegsman/5408ecd397744ba0393e
uint16_t mode_colortwinkle(void) {
    unsigned dataSize = (SEGLEN + 7) >> 3;  //1 bit per LED
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    CRGB   fastled_col, prev;
    fract8 fadeUpAmount   = strip.getBrightness() > 28 ? 8 + (SEGMENT.speed >> 2) : 68 - strip.getBrightness();
    fract8 fadeDownAmount = strip.getBrightness() > 28 ? 8 + (SEGMENT.speed >> 3) : 68 - strip.getBrightness();
    for (int i = 0; i < SEGLEN; i++) {
        fastled_col     = SEGMENT.getPixelColor(i);
        prev            = fastled_col;
        unsigned index  = i >> 3;
        unsigned bitNum = i & 0x07;
        bool     fadeUp = bitRead(SEGENV.data[index], bitNum);

        if (fadeUp) {
            CRGB incrementalColor = fastled_col;
            incrementalColor.nscale8_video(fadeUpAmount);
            fastled_col += incrementalColor;

            if (fastled_col.red == 255 || fastled_col.green == 255 || fastled_col.blue == 255) {
                bitWrite(SEGENV.data[index], bitNum, false);
            }
            SEGMENT.setPixelColor(i, fastled_col.red, fastled_col.green, fastled_col.blue);

            if (SEGMENT.getPixelColor(i) == RGBW32(prev.r, prev.g, prev.b, 0)) {  //fix "stuck" pixels
                fastled_col += fastled_col;
                SEGMENT.setPixelColor(i, fastled_col);
            }
        } else {
            fastled_col.nscale8(255 - fadeDownAmount);
            SEGMENT.setPixelColor(i, fastled_col);
        }
    }

    for (unsigned j = 0; j <= SEGLEN / 50; j++) {
        if (random8() <= SEGMENT.intensity) {
            for (unsigned times = 0; times < 5; times++) {  //attempt to spawn a new pixel 5 times
                int i = random16(SEGLEN);
                if (SEGMENT.getPixelColor(i) == 0) {
                    fastled_col     = ColorFromPalette(SEGPALETTE, random8(), 64, NOBLEND);
                    unsigned index  = i >> 3;
                    unsigned bitNum = i & 0x07;
                    bitWrite(SEGENV.data[index], bitNum, true);
                    SEGMENT.setPixelColor(i, fastled_col);
                    break;  //only spawn 1 new pixel per frame per 50 LEDs
                }
            }
        }
    }
    return FRAMETIME_FIXED;
}

// ---- Lake ---------------------------------------------------------------------------
//Calm effect, like a lake at night
uint16_t mode_lake(void) {
    unsigned sp    = SEGMENT.speed / 10;
    int      wave1 = beatsin8_t(sp + 2, -64, 64);
    int      wave2 = beatsin8_t(sp + 1, -64, 64);
    int      wave3 = beatsin8_t(sp + 2, 0, 80);

    for (int i = 0; i < SEGLEN; i++) {
        int     index = cos8_t((i * 15) + wave1) / 2 + cubicwave8((i * 23) + wave2) / 2;
        uint8_t lum   = (index > wave3) ? index - wave3 : 0;
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, false, 0, lum));
    }

    return FRAMETIME;
}

// ---- Meteor Smooth ------------------------------------------------------------------
// send a meteor from begining to to the end of the strip with a trail that randomly decays.
// adapted from https://www.tweaking4all.com/hardware/arduino/adruino-led-strip-effects/#LEDStripEffectMeteorRain
uint16_t mode_meteor_smooth(void) {
    if (SEGLEN == 1) return mode_static();
    if (!SEGENV.allocateData(SEGLEN)) return mode_static();  //allocation failed

    byte* trail = SEGENV.data;

    const unsigned meteorSize = 1 + SEGLEN / 20;  // 5%
    uint16_t       in         = map((SEGENV.step >> 6 & 0xFF), 0, 255, 0, SEGLEN - 1);

    const int max = SEGMENT.palette == 5 || !SEGMENT.check1 ? 240 : 255;
    // fade all leds to colors[1] in LEDs one step
    for (unsigned i = 0; i < SEGLEN; i++) {
        if (/*trail[i] != 0 &&*/ random8() <= 255 - SEGMENT.intensity) {
            int change = trail[i] + 4 - random8(24);  //change each time between -20 and +4
            trail[i]   = constrain(change, 0, max);
            uint32_t col = SEGMENT.check1 ? SEGMENT.color_from_palette(i, true, false, 0, trail[i])
                                          : SEGMENT.color_from_palette(trail[i], false, true, 255);
            SEGMENT.setPixelColor(i, col);
        }
    }

    // draw meteor
    for (unsigned j = 0; j < meteorSize; j++) {
        unsigned index = in + j;
        if (index >= SEGLEN) {
            index -= SEGLEN;
        }
        trail[index] = max;
        uint32_t col = SEGMENT.check1 ? SEGMENT.color_from_palette(index, true, false, 0, trail[index])
                                      : SEGMENT.color_from_palette(trail[index], false, true, 255);
        SEGMENT.setPixelColor(index, col);
    }

    SEGENV.step += SEGMENT.speed + 1;
    return FRAMETIME;
}

// ---- Twinklefox / Twinklecat ------------------------------------------------------
//  TwinkleFOX by Mark Kriegsman: https://gist.github.com/kriegsman/756ea6dcae8e30845b5a
//  Twinkling 'holiday' lights that fade in and out. Colors are chosen from a palette.
static CRGB twinklefox_one_twinkle(uint32_t ms, uint8_t salt, bool cat) {
    // Overall twinkle speed (changed)
    unsigned ticks       = ms / SEGENV.aux0;
    unsigned fastcycle8  = uint8_t(ticks);
    uint16_t slowcycle16 = (ticks >> 8) + salt;
    slowcycle16 += sin8_t(slowcycle16);
    slowcycle16        = (slowcycle16 * 2053) + 1384;
    uint8_t slowcycle8 = (slowcycle16 & 0xFF) + (slowcycle16 >> 8);

    // Overall twinkle density: 0 (NONE lit) to 8 (ALL lit at once).
    unsigned twinkleDensity = (SEGMENT.intensity >> 5) + 1;

    unsigned bright = 0;
    if (((slowcycle8 & 0x0E) / 2) < twinkleDensity) {
        unsigned ph = fastcycle8;
        // like 'triwave8', but with a faster attack and a slower decay
        if (cat)  //twinklecat, variant where the leds instantly turn on
        {
            bright = 255 - ph;
        } else {  //vanilla twinklefox
            if (ph < 86) {
                bright = ph * 3;
            } else {
                ph -= 86;
                bright = 255 - (ph + (ph / 2));
            }
        }
    }

    unsigned hue = slowcycle8 - salt;
    CRGB     c;
    if (bright > 0) {
        c = ColorFromPalette(SEGPALETTE, hue, bright, NOBLEND);
        if (!SEGMENT.check1) {
            // A pixel in the 'fading down' part of the cycle fades toward 'red', like an incandescent bulb.
            if (fastcycle8 >= 128) {
                unsigned cooling = (fastcycle8 - 128) >> 4;
                c.g              = qsub8(c.g, cooling);
                c.b              = qsub8(c.b, cooling * 2);
            }
        }
    } else {
        c = CRGB::Black;
    }
    return c;
}

//  Loops over each pixel, calculates the adjusted 'clock' that this pixel should use, and calls
//  "CalculateOneTwinkle" on each pixel.  It then displays either the twinkle color or the
//  background color, whichever is brighter.
static uint16_t twinklefox_base(bool cat) {
    // "PRNG16" MUST be reset to the same starting value each time, so that the sequence of
    // 'random' numbers it generates is (paradoxically) stable.
    uint16_t PRNG16 = 11337;

    // Calculate speed
    if (SEGMENT.speed > 100)
        SEGENV.aux0 = 3 + ((255 - SEGMENT.speed) >> 3);
    else
        SEGENV.aux0 = 22 + ((100 - SEGMENT.speed) >> 1);

    // Set up the background color, "bg".
    CRGB     bg       = CRGB(SEGCOLOR(1));
    unsigned bglight  = bg.getAverageLight();
    if (bglight > 64) {
        bg.nscale8_video(16);  // very bright, so scale to 1/16th
    } else if (bglight > 16) {
        bg.nscale8_video(64);  // not that bright, so scale to 1/4th
    } else {
        bg.nscale8_video(86);  // dim, scale to 1/3rd.
    }

    unsigned backgroundBrightness = bg.getAverageLight();

    for (int i = 0; i < SEGLEN; i++) {
        PRNG16                   = (uint16_t)(PRNG16 * 2053) + 1384;  // next 'random' number
        unsigned myclockoffset16 = PRNG16;                             // use that number as clock offset
        PRNG16                   = (uint16_t)(PRNG16 * 2053) + 1384;  // next 'random' number
        // use that number as clock speed adjustment factor (in 8ths, from 8/8ths to 23/8ths)
        unsigned myspeedmultiplierQ5_3 = ((((PRNG16 & 0xFF) >> 4) + (PRNG16 & 0x0F)) & 0x0F) + 0x08;
        uint32_t myclock30             = (uint32_t)((strip.now * myspeedmultiplierQ5_3) >> 3) + myclockoffset16;
        unsigned myunique8             = PRNG16 >> 8;  // get 'salt' value for this pixel

        // We now have the adjusted 'clock' for this pixel; compute its color as "brightness = f( time )".
        CRGB c = twinklefox_one_twinkle(myclock30, myunique8, cat);

        unsigned cbright     = c.getAverageLight();
        int      deltabright = cbright - backgroundBrightness;
        if (deltabright >= 32 || (!bg)) {
            // significantly brighter than the background color: use the new color.
            SEGMENT.setPixelColor(i, c.red, c.green, c.blue);
        } else if (deltabright > 0) {
            // just slightly brighter than the background color: mix the new color and the background color
            SEGMENT.setPixelColor(i, color_blend(RGBW32(bg.r, bg.g, bg.b, 0), RGBW32(c.r, c.g, c.b, 0), deltabright * 8));
        } else {
            // not at all brighter than the background color: just use the background color.
            SEGMENT.setPixelColor(i, bg.r, bg.g, bg.b);
        }
    }
    return FRAMETIME;
}

uint16_t mode_twinklefox(void) {
    return twinklefox_base(false);
}

uint16_t mode_twinklecat(void) {
    return twinklefox_base(true);
}

// ---- Solid Pattern ------------------------------------------------------------------
//Speed slider sets amount of LEDs lit, intensity sets unlit
uint16_t mode_static_pattern(void) {
    unsigned lit        = 1 + SEGMENT.speed;
    unsigned unlit      = 1 + SEGMENT.intensity;
    bool     drawingLit = true;
    unsigned cnt        = 0;

    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, (drawingLit) ? SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0) : SEGCOLOR(1));
        cnt++;
        if (cnt >= ((drawingLit) ? lit : unlit)) {
            cnt        = 0;
            drawingLit = !drawingLit;
        }
    }

    return FRAMETIME;
}

// ---- Solid Pattern Tri --------------------------------------------------------------
uint16_t mode_tri_static_pattern(void) {
    unsigned segSize      = (SEGMENT.intensity >> 5) + 1;
    unsigned currSeg      = 0;
    unsigned currSegCount = 0;

    for (int i = 0; i < SEGLEN; i++) {
        if (currSeg % 3 == 0) {
            SEGMENT.setPixelColor(i, SEGCOLOR(0));
        } else if (currSeg % 3 == 1) {
            SEGMENT.setPixelColor(i, SEGCOLOR(1));
        } else {
            SEGMENT.setPixelColor(i, SEGCOLOR(2));
        }
        currSegCount += 1;
        if (currSegCount >= segSize) {
            currSeg += 1;
            currSegCount = 0;
        }
    }

    return FRAMETIME;
}

// ---- Spots / Spots Fade -------------------------------------------------------------
static uint16_t spots_base(uint16_t threshold) {
    if (SEGLEN == 1) return mode_static();
    if (!SEGMENT.check2) SEGMENT.fill(SEGCOLOR(1));

    unsigned maxZones = SEGLEN >> 2;
    unsigned zones    = 1 + ((SEGMENT.intensity * maxZones) >> 8);
    unsigned zoneLen  = SEGLEN / zones;
    unsigned offset   = (SEGLEN - zones * zoneLen) >> 1;

    for (unsigned z = 0; z < zones; z++) {
        unsigned pos = offset + z * zoneLen;
        for (unsigned i = 0; i < zoneLen; i++) {
            unsigned wave = triwave16((i * 0xFFFF) / zoneLen);
            if (wave > threshold) {
                unsigned index = 0 + pos + i;
                unsigned s     = (wave - threshold) * 255 / (0xFFFF - threshold);
                SEGMENT.setPixelColor(index, color_blend(SEGMENT.color_from_palette(index, true, PALETTE_SOLID_WRAP, 0), SEGCOLOR(1), 255 - s));
            }
        }
    }

    return FRAMETIME;
}

//Intensity slider sets number of "lights", speed sets LEDs per light
uint16_t mode_spots(void) {
    return spots_base((255 - SEGMENT.speed) << 8);
}

//Intensity slider sets number of "lights", LEDs per light fade in and out
uint16_t mode_spots_fade(void) {
    unsigned counter = strip.now * ((SEGMENT.speed >> 2) + 8);
    unsigned t       = triwave16(counter);
    unsigned tr      = (t >> 1) + (t >> 2);
    return spots_base(tr);
}

}  // namespace wled
