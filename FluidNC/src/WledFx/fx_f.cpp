// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_f.ts; tools/fx_parity.py checks them.
//
// Batch f: Sunrise, Phased, Phased Noise, Twinkleup, Noise Pal, Flow, Chunchun,
// Dancing Shadows, Washing Machine, Blends, TV Simulator, Perlin Move,
// Flow Stripe, Wavesins.
#include "WledFx.h"

namespace wled {

// FastLED's CRGBPalette16(const CHSV& c1..c4): an HSV gradient (fill_gradient,
// SHORTEST_HUES) converted to RGB per entry.  The layer only has the CRGB
// constructors, which a CHSV argument would silently convert to (an RGB
// gradient: different colors).
static void fill_gradient_hsv(CRGB* targetArray, uint16_t startpos, CHSV startcolor, uint16_t endpos, CHSV endcolor) {
    if (endpos < startpos) {
        uint16_t t  = endpos;
        CHSV     tc = endcolor;
        endcolor    = startcolor;
        endpos      = startpos;
        startpos    = t;
        startcolor  = tc;
    }
    if (endcolor.val == 0 || endcolor.sat == 0) {
        endcolor.hue = startcolor.hue;
    }
    if (startcolor.val == 0 || startcolor.sat == 0) {
        startcolor.hue = endcolor.hue;
    }
    saccum87 huedistance87;
    saccum87 satdistance87;
    saccum87 valdistance87;
    // FastLED shifts the signed difference `<< 7`: UB on a negative value before C++20
    satdistance87     = (endcolor.sat - startcolor.sat) * 128;
    valdistance87     = (endcolor.val - startcolor.val) * 128;
    uint8_t huedelta8 = endcolor.hue - startcolor.hue;
    if (huedelta8 > 127) {  // SHORTEST_HUES -> BACKWARD_HUES
        huedistance87 = (uint8_t)(256 - huedelta8) << 7;
        huedistance87 = -huedistance87;
    } else {  // FORWARD_HUES
        huedistance87 = huedelta8 << 7;
    }
    uint16_t pixeldistance = endpos - startpos;
    int16_t  divisor       = pixeldistance ? pixeldistance : 1;
    saccum87 huedelta87    = huedistance87 / divisor;
    saccum87 satdelta87    = satdistance87 / divisor;
    saccum87 valdelta87    = valdistance87 / divisor;
    huedelta87 *= 2;
    satdelta87 *= 2;
    valdelta87 *= 2;
    accum88 hue88 = startcolor.hue << 8;
    accum88 sat88 = startcolor.sat << 8;
    accum88 val88 = startcolor.val << 8;
    for (uint16_t i = startpos; i <= endpos; ++i) {
        targetArray[i] = CHSV(hue88 >> 8, sat88 >> 8, val88 >> 8);
        hue88 += huedelta87;
        sat88 += satdelta87;
        val88 += valdelta87;
    }
}

static CRGBPalette16 hsvPalette16(const CHSV& c1, const CHSV& c2, const CHSV& c3, const CHSV& c4) {
    CRGBPalette16 p;
    uint16_t      onethird  = (16 / 3);
    uint16_t      twothirds = ((16 * 2) / 3);
    uint16_t      last      = 16 - 1;
    fill_gradient_hsv(p.entries, 0, c1, onethird, c2);
    fill_gradient_hsv(p.entries, onethird, c2, twothirds, c3);
    fill_gradient_hsv(p.entries, twothirds, c3, last, c4);
    return p;
}

// FX.cpp's own helper (not part of the layer)
static int8_t tristate_square8(uint8_t x, uint8_t pulsewidth, uint8_t attdec) {
    int8_t a = 127;
    if (x > 127) {
        a = -127;
        x -= 127;
    }

    if (x < attdec) {  //inc to max
        return (int16_t)x * a / attdec;
    } else if (x < pulsewidth - attdec) {  //max
        return a;
    } else if (x < pulsewidth) {  //dec to 0
        return (int16_t)(pulsewidth - x) * a / attdec;
    }
    return 0;
}

// ---- Sunrise ----------------------------------------------------------------
uint16_t mode_sunrise(void) {
    if (SEGLEN == 1) return mode_static();
    //speed 0 - static sun
    //speed 1 - 60: sunrise time in minutes
    //speed 60 - 120 : sunset time in minutes - 60;
    //speed above: "breathing" rise and set
    if (SEGENV.call == 0 || SEGMENT.speed != SEGENV.aux0) {
        SEGENV.step = millis();  //save starting time, millis() because strip.now can change from sync
        SEGENV.aux0 = SEGMENT.speed;
    }

    SEGMENT.fill(BLACK);
    unsigned stage = 0xFFFF;

    uint32_t s10SinceStart = (millis() - SEGENV.step) / 100;  //tenths of seconds

    if (SEGMENT.speed > 120) {  //quick sunrise and sunset
        unsigned counter = (strip.now >> 1) * (((SEGMENT.speed - 120) >> 1) + 1);
        stage            = triwave16(counter);
    } else if (SEGMENT.speed) {  //sunrise
        unsigned durMins = SEGMENT.speed;
        if (durMins > 60) durMins -= 60;
        uint32_t s10Target = durMins * 600;
        if (s10SinceStart > s10Target) s10SinceStart = s10Target;
        stage = map(s10SinceStart, 0, s10Target, 0, 0xFFFF);
        if (SEGMENT.speed > 60) stage = 0xFFFF - stage;  //sunset
    }

    for (int i = 0; i <= SEGLEN / 2; i++) {
        //default palette is Fire
        uint32_t c = SEGMENT.color_from_palette(0, false, true, 255);  //background

        unsigned wave = triwave16((i * stage) / SEGLEN);

        wave = (wave >> 8) + ((wave * SEGMENT.intensity) >> 15);

        if (wave > 240) {  //clipped, full white sun
            c = SEGMENT.color_from_palette(240, false, true, 255);
        } else {  //transition
            c = SEGMENT.color_from_palette(wave, false, true, 255);
        }
        SEGMENT.setPixelColor(i, c);
        SEGMENT.setPixelColor(SEGLEN - i - 1, c);
    }

    return FRAMETIME;
}

// ---- Phased / Phased Noise ----------------------------------------------------
static uint16_t phased_base(uint8_t moder) {  // We're making sine waves here. By Andrew Tuline.

    unsigned allfreq = 16;                                   // Base frequency.
    float*   phase   = reinterpret_cast<float*>(&SEGENV.step);  // Phase change value gets calculated (float fits into unsigned long).
    unsigned cutOff  = (255 - SEGMENT.intensity);            // You can change the number of pixels.  AKA INTENSITY (was 192).
    unsigned modVal  = 5;                                    //SEGMENT.fft1/8+1;                         // You can change the modulus. AKA FFT1 (was 5).

    unsigned index = strip.now / 64;  // Set color rotation speed
    *phase += SEGMENT.speed / 32.0;   // You can change the speed of the wave. AKA SPEED (was .4)

    for (int i = 0; i < SEGLEN; i++) {
        if (moder == 1) modVal = (inoise8(i * 10 + i * 10) / 16);  // Let's randomize our mod length with some Perlin noise.
        unsigned val = (i + 1) * allfreq;                          // This sets the frequency of the waves. The +1 makes sure that led 0 is used.
        if (modVal == 0) modVal = 1;
        val += *phase * (i % modVal + 1) / 2;  // This sets the varying phase change of the waves. By Andrew Tuline.
        unsigned b = cubicwave8(val);          // Now we make an 8 bit sinewave.
        b          = (b > cutOff) ? (b - cutOff) : 0;  // A ternary operator to cutoff the light.
        SEGMENT.setPixelColor(i, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(index, false, false, 0), b));
        index += 256 / SEGLEN;
        if (SEGLEN > 256) index++;  // Correction for segments longer than 256 LEDs
    }

    return FRAMETIME;
}

uint16_t mode_phased(void) {
    return phased_base(0);
}

uint16_t mode_phased_noise(void) {
    return phased_base(1);
}

// ---- Twinkleup ----------------------------------------------------------------
uint16_t mode_twinkleup(void) {               // A very short twinkle routine with fade-in and dual controls. By Andrew Tuline.
    unsigned prevSeed = random16_get_seed();  // save seed so we can restore it at the end of the function
    random16_set_seed(535);                   // The randomizer needs to be re-set each time through the loop in order for the same 'random' numbers to be the same each time through.

    for (int i = 0; i < SEGLEN; i++) {
        unsigned ranstart = random8();  // The starting value (aka brightness) for each pixel. Must be consistent each time through the loop for this to work.
        unsigned pixBri   = sin8_t(ranstart + 16 * strip.now / (256 - SEGMENT.speed));
        if (random8() > SEGMENT.intensity) pixBri = 0;
        SEGMENT.setPixelColor(i, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(random8() + strip.now / 100, false, PALETTE_SOLID_WRAP, 0), pixBri));
    }

    random16_set_seed(prevSeed);  // restore original seed so other effects can use "random" PRNG
    return FRAMETIME;
}

// ---- Noise Pal ----------------------------------------------------------------
// Peaceful noise that's slow and with gradually changing palettes. Does not support WLED palettes or default colours or controls.
uint16_t mode_noisepal(void) {                       // Slow noise palette by Andrew Tuline.
    unsigned scale = 15 + (SEGMENT.intensity >> 2);  //default was 30
    //#define scale 30

    unsigned dataSize = sizeof(CRGBPalette16) * 2;              //allocate space for 2 Palettes (2 * 16 * 3 = 96 bytes)
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed

    CRGBPalette16* palettes = reinterpret_cast<CRGBPalette16*>(SEGENV.data);

    unsigned changePaletteMs = 4000 + SEGMENT.speed * 10;  //between 4 - 6.5sec
    if (strip.now - SEGENV.step > changePaletteMs) {
        SEGENV.step = strip.now;

        unsigned baseI = random8();
        // sequenced: the argument order is unspecified in C++
        uint8_t h1  = random8(64);
        uint8_t v1  = random8(128, 255);
        uint8_t v2  = random8(128, 255);
        uint8_t h3  = random8(92);
        uint8_t v3  = random8(128, 255);
        uint8_t h4  = random8(92);
        uint8_t v4  = random8(128, 255);
        palettes[1] = hsvPalette16(CHSV(baseI + h1, 255, v1), CHSV(baseI + 128, 255, v2), CHSV(baseI + h3, 192, v3), CHSV(baseI + h4, 255, v4));
    }

    CRGB color;

    //EVERY_N_MILLIS(10) { //(don't have to time this, effect function is only called every 24ms)
    nblendPaletteTowardPalette(palettes[0], palettes[1], 48);  // Blend towards the target palette over 48 iterations.

    if (SEGMENT.palette > 0) palettes[0] = SEGPALETTE;

    for (int i = 0; i < SEGLEN; i++) {
        unsigned index = inoise8(i * scale, SEGENV.aux0 + i * scale);  // Get a value from the noise function. I'm using both x and y axis.
        color          = ColorFromPalette(palettes[0], index, 255, LINEARBLEND);  // Use the my own palette.
        SEGMENT.setPixelColor(i, color.red, color.green, color.blue);
    }

    SEGENV.aux0 += beatsin8_t(10, 1, 4);  // Moving along the distance. Vary it a bit with a sine wave.

    return FRAMETIME;
}

// ---- Flow ---------------------------------------------------------------------
uint16_t mode_flow(void) {
    unsigned counter = 0;
    if (SEGMENT.speed != 0) {
        counter = strip.now * ((SEGMENT.speed >> 2) + 1);
        counter = counter >> 8;
    }

    unsigned maxZones = SEGLEN / 6;  //only looks good if each zone has at least 6 LEDs
    unsigned zones    = (SEGMENT.intensity * maxZones) >> 8;
    if (zones & 0x01) zones++;  //zones must be even
    if (zones < 2) zones = 2;
    unsigned zoneLen = SEGLEN / zones;
    unsigned offset  = (SEGLEN - zones * zoneLen) >> 1;

    SEGMENT.fill(SEGMENT.color_from_palette(-counter, false, true, 255));

    for (unsigned z = 0; z < zones; z++) {
        unsigned pos = offset + z * zoneLen;
        for (unsigned i = 0; i < zoneLen; i++) {
            unsigned colorIndex = (i * 255 / zoneLen) - counter;
            unsigned led        = (z & 0x01) ? i : (zoneLen - 1) - i;
            // WLED: if (SEGMENT.reverse) led = (zoneLen -1) -led;  -- no segment options here (never reversed)
            SEGMENT.setPixelColor(pos + led, SEGMENT.color_from_palette(colorIndex, false, true, 255));
        }
    }

    return FRAMETIME;
}

// ---- Chunchun -----------------------------------------------------------------
uint16_t mode_chunchun(void) {
    if (SEGLEN == 1) return mode_static();
    SEGMENT.fade_out(254);  // add a bit of trail
    unsigned counter  = strip.now * (6 + (SEGMENT.speed >> 4));
    unsigned numBirds = 2 + (SEGLEN >> 3);  // 2 + 1/8 of a segment
    unsigned span     = (SEGMENT.intensity << 8) / numBirds;

    for (unsigned i = 0; i < numBirds; i++) {
        counter -= span;
        unsigned megumin = sin16_t(counter) + 0x8000;
        unsigned bird    = uint32_t(megumin * SEGLEN) >> 16;
        uint32_t c       = SEGMENT.color_from_palette((i * 255) / numBirds, false, false, 0);  // no palette wrapping
        bird             = constrain(bird, 0U, SEGLEN - 1U);
        SEGMENT.setPixelColor(bird, c);
    }
    return FRAMETIME;
}

// ---- Dancing Shadows ----------------------------------------------------------
namespace {
//13 bytes
typedef struct Spotlight {
    float         speed;
    uint8_t       colorIdx;
    int16_t       position;
    unsigned long lastUpdateTime;
    uint8_t       width;
    uint8_t       type;
} spotlight;
}  // namespace

#define SPOT_TYPE_SOLID 0
#define SPOT_TYPE_GRADIENT 1
#define SPOT_TYPE_2X_GRADIENT 2
#define SPOT_TYPE_2X_DOT 3
#define SPOT_TYPE_3X_DOT 4
#define SPOT_TYPE_4X_DOT 5
#define SPOT_TYPES_COUNT 6
#define SPOT_MAX_COUNT 49  //Number of simultaneous waves (ESP32)

/*
 * Spotlights moving back and forth that cast dancing shadows.
 * Shine this through tree branches/leaves or other close-up objects that cast
 * interesting shadows onto a ceiling or tarp.
 *
 * By Steve Pomeroy @xxv
 */
uint16_t mode_dancing_shadows(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned numSpotlights = map(SEGMENT.intensity, 0, 255, 2, SPOT_MAX_COUNT);  // 49 on 32 segment ESP32, 17 on 16 segment ESP8266
    bool     initialize    = SEGENV.aux0 != numSpotlights;
    SEGENV.aux0            = numSpotlights;

    unsigned dataSize = sizeof(spotlight) * numSpotlights;
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed
    Spotlight* spotlights = reinterpret_cast<Spotlight*>(SEGENV.data);

    SEGMENT.fill(BLACK);

    unsigned long time    = strip.now;
    bool          respawn = false;

    for (size_t i = 0; i < numSpotlights; i++) {
        if (!initialize) {
            // advance the position of the spotlight
            int delta = (float)(time - spotlights[i].lastUpdateTime) * (spotlights[i].speed * ((1.0 + SEGMENT.speed) / 100.0));

            if (abs(delta) >= 1) {
                spotlights[i].position += delta;
                spotlights[i].lastUpdateTime = time;
            }

            respawn = (spotlights[i].speed > 0.0 && spotlights[i].position > (SEGLEN + 2)) ||
                      (spotlights[i].speed < 0.0 && spotlights[i].position < -(spotlights[i].width + 2));
        }

        if (initialize || respawn) {
            spotlights[i].colorIdx = random8();
            spotlights[i].width    = random8(1, 10);

            spotlights[i].speed = 1.0 / random8(4, 50);

            if (initialize) {
                spotlights[i].position = random16(SEGLEN);
                spotlights[i].speed *= random8(2) ? 1.0 : -1.0;
            } else {
                if (random8(2)) {
                    spotlights[i].position = SEGLEN + spotlights[i].width;
                    spotlights[i].speed *= -1.0;
                } else {
                    spotlights[i].position = -spotlights[i].width;
                }
            }

            spotlights[i].lastUpdateTime = time;
            spotlights[i].type           = random8(SPOT_TYPES_COUNT);
        }

        uint32_t color = SEGMENT.color_from_palette(spotlights[i].colorIdx, false, false, 255);
        int      start = spotlights[i].position;

        if (spotlights[i].width <= 1) {
            if (start >= 0 && start < SEGLEN) {
                SEGMENT.blendPixelColor(start, color, 128);
            }
        } else {
            switch (spotlights[i].type) {
                case SPOT_TYPE_SOLID:
                    for (size_t j = 0; j < spotlights[i].width; j++) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, 128);
                        }
                    }
                    break;

                case SPOT_TYPE_GRADIENT:
                    for (size_t j = 0; j < spotlights[i].width; j++) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, cubicwave8(map(j, 0, spotlights[i].width - 1, 0, 255)));
                        }
                    }
                    break;

                case SPOT_TYPE_2X_GRADIENT:
                    for (size_t j = 0; j < spotlights[i].width; j++) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, cubicwave8(2 * map(j, 0, spotlights[i].width - 1, 0, 255)));
                        }
                    }
                    break;

                case SPOT_TYPE_2X_DOT:
                    for (size_t j = 0; j < spotlights[i].width; j += 2) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, 128);
                        }
                    }
                    break;

                case SPOT_TYPE_3X_DOT:
                    for (size_t j = 0; j < spotlights[i].width; j += 3) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, 128);
                        }
                    }
                    break;

                case SPOT_TYPE_4X_DOT:
                    for (size_t j = 0; j < spotlights[i].width; j += 4) {
                        if ((start + j) >= 0 && (start + j) < SEGLEN) {
                            SEGMENT.blendPixelColor(start + j, color, 128);
                        }
                    }
                    break;
            }
        }
    }

    return FRAMETIME;
}

// ---- Washing Machine ------------------------------------------------------------
/*
  Imitates a washing machine, rotating same waves forward, then pause, then backward.
  By Stefan Seegel
*/
uint16_t mode_washing_machine(void) {
    int speed = tristate_square8(strip.now >> 7, 90, 15);

    SEGENV.step += (speed * 2048) / (512 - SEGMENT.speed);

    for (int i = 0; i < SEGLEN; i++) {
        uint8_t col = sin8_t(((SEGMENT.intensity / 25 + 1) * 255 * i / SEGLEN) + (SEGENV.step >> 7));
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(col, false, PALETTE_SOLID_WRAP, 3));
    }

    return FRAMETIME;
}

// ---- Blends ---------------------------------------------------------------------
/*
  Blends random colors across palette
  Modified, originally by Mark Kriegsman https://gist.github.com/kriegsman/1f7ccbbfa492a73c015e
*/
uint16_t mode_blends(void) {
    unsigned pixelLen = SEGLEN > UINT8_MAX ? UINT8_MAX : SEGLEN;
    unsigned dataSize = sizeof(uint32_t) * (pixelLen + 1);  // max segment length of 56 pixels on 16 segment ESP8266
    if (!SEGENV.allocateData(dataSize)) return mode_static();  //allocation failed
    uint32_t* pixels     = reinterpret_cast<uint32_t*>(SEGENV.data);
    unsigned  blendSpeed = map(SEGMENT.intensity, 0, UINT8_MAX, 10, 128);
    unsigned  shift      = (strip.now * ((SEGMENT.speed >> 3) + 1)) >> 8;

    for (unsigned i = 0; i < pixelLen; i++) {
        pixels[i] = color_blend(pixels[i], SEGMENT.color_from_palette(shift + quadwave8((i + 1) * 16), false, PALETTE_SOLID_WRAP, 255), blendSpeed);
        shift += 3;
    }

    unsigned offset = 0;
    for (unsigned i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, pixels[offset++]);
        if (offset > pixelLen) offset = 0;
    }

    return FRAMETIME;
}

// ---- TV Simulator -----------------------------------------------------------------
/*
  TV Simulator
  Modified and adapted to WLED by Def3nder, based on "Fake TV Light for Engineers" by Phillip Burgess https://learn.adafruit.com/fake-tv-light-for-engineers/arduino-sketch
*/
namespace {
//43 bytes
typedef struct TvSim {
    uint32_t totalTime      = 0;
    uint32_t fadeTime       = 0;
    uint32_t startTime      = 0;
    uint32_t elapsed        = 0;
    uint32_t pixelNum       = 0;
    uint16_t sliderValues   = 0;
    uint32_t sceeneStart    = 0;
    uint32_t sceeneDuration = 0;
    uint16_t sceeneColorHue = 0;
    uint8_t  sceeneColorSat = 0;
    uint8_t  sceeneColorBri = 0;
    uint8_t  actualColorR   = 0;
    uint8_t  actualColorG   = 0;
    uint8_t  actualColorB   = 0;
    uint16_t pr             = 0;  // Prev R, G, B
    uint16_t pg             = 0;
    uint16_t pb             = 0;
} tvSim;
}  // namespace

uint16_t mode_tv_simulator(void) {
    int     nr, ng, nb, r, g, b, i, hue;
    uint8_t sat, bri, j;

    if (!SEGENV.allocateData(sizeof(tvSim))) return mode_static();  //allocation failed
    TvSim* tvSimulator = reinterpret_cast<TvSim*>(SEGENV.data);

    uint8_t colorSpeed     = map(SEGMENT.speed, 0, UINT8_MAX, 1, 20);
    uint8_t colorIntensity = map(SEGMENT.intensity, 0, UINT8_MAX, 10, 30);

    i = SEGMENT.speed << 8 | SEGMENT.intensity;
    if (i != tvSimulator->sliderValues) {
        tvSimulator->sliderValues = i;
        SEGENV.aux1               = 0;
    }

    // create a new sceene
    if (((strip.now - tvSimulator->sceeneStart) >= tvSimulator->sceeneDuration) || SEGENV.aux1 == 0) {
        tvSimulator->sceeneStart    = strip.now;                                             // remember the start of the new sceene
        tvSimulator->sceeneDuration = random16(60 * 250 * colorSpeed, 60 * 750 * colorSpeed);  // duration of a "movie sceene" which has similar colors (5 to 15 minutes with max speed slider)
        tvSimulator->sceeneColorHue = random16(0, 768);                                        // random start color-tone for the sceene
        tvSimulator->sceeneColorSat = random8(100, 130 + colorIntensity);                      // random start color-saturation for the sceene
        tvSimulator->sceeneColorBri = random8(200, 240);                                       // random start color-brightness for the sceene
        SEGENV.aux1                 = 1;
        SEGENV.aux0                 = 0;
    }

    // slightly change the color-tone in this sceene
    if (SEGENV.aux0 == 0) {
        // hue change in both directions
        j   = random8(4 * colorIntensity);
        hue = (random8() < 128) ? ((j < tvSimulator->sceeneColorHue) ? tvSimulator->sceeneColorHue - j : 767 - tvSimulator->sceeneColorHue - j) :  // negative
                  ((j + tvSimulator->sceeneColorHue) < 767 ? tvSimulator->sceeneColorHue + j : tvSimulator->sceeneColorHue + j - 767);  // positive

        // saturation
        j   = random8(2 * colorIntensity);
        sat = (tvSimulator->sceeneColorSat - j) < 0 ? 0 : tvSimulator->sceeneColorSat - j;

        // brightness
        j   = random8(100);
        bri = (tvSimulator->sceeneColorBri - j) < 0 ? 0 : tvSimulator->sceeneColorBri - j;

        // calculate R,G,B from HSV
        // Source: https://blog.adafruit.com/2012/03/14/constant-brightness-hsb-to-rgb-algorithm/
        {  // just to create a local scope for  the variables
            uint8_t temp[5], n = (hue >> 8) % 3;
            uint8_t x = ((((hue & 255) * sat) >> 8) * bri) >> 8;
            uint8_t s = ((256 - sat) * bri) >> 8;
            temp[0] = temp[3]         = s;
            temp[1] = temp[4]         = x + s;
            temp[2]                   = bri - x;
            tvSimulator->actualColorR = temp[n + 2];
            tvSimulator->actualColorG = temp[n + 1];
            tvSimulator->actualColorB = temp[n];
        }
    }
    // Apply gamma correction, further expand to 16/16/16
    nr = (uint8_t)gamma8(tvSimulator->actualColorR) * 257;  // New R/G/B
    ng = (uint8_t)gamma8(tvSimulator->actualColorG) * 257;
    nb = (uint8_t)gamma8(tvSimulator->actualColorB) * 257;

    if (SEGENV.aux0 == 0) {  // initialize next iteration
        SEGENV.aux0 = 1;

        // randomize total duration and fade duration for the actual color
        tvSimulator->totalTime = random16(250, 2500);                  // Semi-random pixel-to-pixel time
        tvSimulator->fadeTime  = random16(0, tvSimulator->totalTime);  // Pixel-to-pixel transition time
        if (random8(10) < 3) tvSimulator->fadeTime = 0;                // Force scene cut 30% of time

        tvSimulator->startTime = strip.now;
    }  // end of initialization

    // how much time is elapsed ?
    tvSimulator->elapsed = strip.now - tvSimulator->startTime;

    // fade from prev color to next color
    if (tvSimulator->elapsed < tvSimulator->fadeTime) {
        r = map(tvSimulator->elapsed, 0, tvSimulator->fadeTime, tvSimulator->pr, nr);
        g = map(tvSimulator->elapsed, 0, tvSimulator->fadeTime, tvSimulator->pg, ng);
        b = map(tvSimulator->elapsed, 0, tvSimulator->fadeTime, tvSimulator->pb, nb);
    } else {  // Avoid divide-by-zero in map()
        r = nr;
        g = ng;
        b = nb;
    }

    // set strip color
    for (i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, r >> 8, g >> 8, b >> 8);  // Quantize to 8-bit
    }

    // if total duration has passed, remember last color and restart the loop
    if (tvSimulator->elapsed >= tvSimulator->totalTime) {
        tvSimulator->pr = nr;  // Prev RGB = new RGB
        tvSimulator->pg = ng;
        tvSimulator->pb = nb;
        SEGENV.aux0     = 0;
    }

    return FRAMETIME;
}

// ---- Perlin Move ----------------------------------------------------------------
// 16 bit perlinmove. Use Perlin Noise instead of sinewaves for movement. By Andrew Tuline.
// Controls are speed, # of pixels, faderate.
uint16_t mode_perlinmove(void) {
    if (SEGLEN == 1) return mode_static();
    SEGMENT.fade_out(255 - SEGMENT.custom1);
    for (int i = 0; i < SEGMENT.intensity / 16 + 1; i++) {
        unsigned locn   = inoise16(strip.now * 128 / (260 - SEGMENT.speed) + i * 15000, strip.now * 128 / (260 - SEGMENT.speed));  // Get a new pixel location from moving noise.
        unsigned pixloc = map(locn, 50 * 256, 192 * 256, 0, SEGLEN - 1);  // Map that to the length of the strand, and ensure we don't go over.
        SEGMENT.setPixelColor(pixloc, SEGMENT.color_from_palette(pixloc % 255, false, PALETTE_SOLID_WRAP, 0));
    }

    return FRAMETIME;
}  // mode_perlinmove()

// ---- Wavesins -------------------------------------------------------------------
// Uses beatsin8() + phase shifting. By: Andrew Tuline
uint16_t mode_wavesins(void) {
    for (int i = 0; i < SEGLEN; i++) {
        uint8_t bri   = sin8_t(strip.now / 4 + i * SEGMENT.intensity);
        uint8_t index = beatsin8_t(SEGMENT.speed, SEGMENT.custom1, SEGMENT.custom1 + SEGMENT.custom2, 0, i * (SEGMENT.custom3 << 3));  // custom3 is reduced resolution slider
        //SEGMENT.setPixelColor(i, ColorFromPalette(SEGPALETTE, index, bri, LINEARBLEND));
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0, bri));
    }

    return FRAMETIME;
}  // mode_waveins()

// ---- Flow Stripe ----------------------------------------------------------------
// By: ldirko  https://editor.soulmatelights.com/gallery/392-flow-led-stripe , modifed by: Andrew Tuline
uint16_t mode_FlowStripe(void) {
    if (SEGLEN == 1) return mode_static();
    const int hl  = SEGLEN * 10 / 13;
    uint8_t   hue = strip.now / (SEGMENT.speed + 1);
    uint32_t  t   = strip.now / (SEGMENT.intensity / 8 + 1);

    for (int i = 0; i < SEGLEN; i++) {
        int c  = (abs(i - hl) / hl) * 127;
        c      = sin8_t(c);
        c      = sin8_t(c / 2 + t);
        byte b = sin8_t(c + t / 8);
        SEGMENT.setPixelColor(i, CHSV(b + hue, 255, 255));
    }

    return FRAMETIME;
}  // mode_FlowStripe()

}  // namespace wled
