// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_b.ts; tools/fx_parity.py checks them.
//
// Batch b: the chases, Colorful, Traffic Light, Chase 2, Stream, the
// scanners, Lighthouse, Rain and Tetrix.
#include "WledFx.h"

namespace wled {

/*
 * color chase function.
 * color1 = background color
 * color2 and color3 = colors of two adjacent leds
 */
// DW: WLED tests `SEGMENT.mode == FX_MODE_CHASE_RANDOM`; there is no mode id here, so the caller says so.
static uint16_t chase(uint32_t color1, uint32_t color2, uint32_t color3, bool do_palette, bool chase_random = false) {
    uint16_t counter = strip.now * ((SEGMENT.speed >> 2) + 1);
    uint16_t a       = (counter * SEGLEN) >> 16;

    if (chase_random) {
        if (a < SEGENV.step)  //we hit the start again, choose new color for Chase random
        {
            SEGENV.aux1 = SEGENV.aux0;  //store previous random color
            SEGENV.aux0 = get_random_wheel_index(SEGENV.aux0);
        }
        color1 = SEGMENT.color_wheel(SEGENV.aux0);
    }
    SEGENV.step = a;

    // Use intensity setting to vary chase up to 1/2 string length
    unsigned size = 1 + ((SEGMENT.intensity * SEGLEN) >> 10);

    uint16_t b = a + size;  //"trail" of chase, filled with color1
    if (b > SEGLEN) b -= SEGLEN;
    uint16_t c = b + size;
    if (c > SEGLEN) c -= SEGLEN;

    //background
    if (do_palette) {
        for (unsigned i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1));
        }
    } else
        SEGMENT.fill(color1);

    //if random, fill old background between a and end
    if (chase_random) {
        color1 = SEGMENT.color_wheel(SEGENV.aux1);
        for (unsigned i = a; i < SEGLEN; i++)
            SEGMENT.setPixelColor(i, color1);
    }

    //fill between points a and b with color2
    if (a < b) {
        for (unsigned i = a; i < b; i++)
            SEGMENT.setPixelColor(i, color2);
    } else {
        for (unsigned i = a; i < SEGLEN; i++)  //fill until end
            SEGMENT.setPixelColor(i, color2);
        for (unsigned i = 0; i < b; i++)  //fill from start until b
            SEGMENT.setPixelColor(i, color2);
    }

    //fill between points b and c with color2
    if (b < c) {
        for (unsigned i = b; i < c; i++)
            SEGMENT.setPixelColor(i, color3);
    } else {
        for (unsigned i = b; i < SEGLEN; i++)  //fill until end
            SEGMENT.setPixelColor(i, color3);
        for (unsigned i = 0; i < c; i++)  //fill from start until c
            SEGMENT.setPixelColor(i, color3);
    }

    return FRAMETIME;
}

/*
 * Primary running followed by random color.
 */
uint16_t mode_chase_random(void) {
    return chase(SEGCOLOR(1), (SEGCOLOR(2)) ? SEGCOLOR(2) : SEGCOLOR(0), SEGCOLOR(0), false, true);
}

/*
 * Primary, secondary running on rainbow.
 */
uint16_t mode_chase_rainbow(void) {
    unsigned color_sep = 256 / SEGLEN;
    if (color_sep == 0) color_sep = 1;  // correction for segments longer than 256 LEDs
    unsigned color_index = SEGENV.call & 0xFF;
    uint32_t color       = SEGMENT.color_wheel(((SEGENV.step * color_sep) + color_index) & 0xFF);

    return chase(color, SEGCOLOR(0), SEGCOLOR(1), false);
}

/*
 * Primary running on rainbow.
 */
uint16_t mode_chase_rainbow_white(void) {
    uint16_t n      = SEGENV.step;
    uint16_t m      = (SEGENV.step + 1) % SEGLEN;
    uint32_t color2 = SEGMENT.color_wheel(((n * 256 / SEGLEN) + (SEGENV.call & 0xFF)) & 0xFF);
    uint32_t color3 = SEGMENT.color_wheel(((m * 256 / SEGLEN) + (SEGENV.call & 0xFF)) & 0xFF);

    return chase(SEGCOLOR(0), color2, color3, false);
}

/*
 * Red - Amber - Green - Blue lights running
 */
uint16_t mode_colorful(void) {
    unsigned numColors = 4;  //3, 4, or 5
    uint32_t cols[9] { 0x00FF0000, 0x00EEBB00, 0x0000EE00, 0x000077CC };
    if (SEGMENT.intensity > 160 || SEGMENT.palette) {  //palette or color
        if (!SEGMENT.palette) {
            numColors = 3;
            for (size_t i = 0; i < 3; i++) cols[i] = SEGCOLOR(i);
        } else {
            unsigned fac = 80;
            if (SEGMENT.palette == 52) {
                numColors = 5;
                fac       = 61;
            }  //C9 2 has 5 colors
            for (size_t i = 0; i < numColors; i++) {
                cols[i] = SEGMENT.color_from_palette(i * fac, false, true, 255);
            }
        }
    } else if (SEGMENT.intensity < 80)  //pastel (easter) colors
    {
        cols[0] = 0x00FF8040;
        cols[1] = 0x00E5D241;
        cols[2] = 0x0077FF77;
        cols[3] = 0x0077F0F0;
    }
    for (size_t i = numColors; i < numColors * 2 - 1U; i++) cols[i] = cols[i - numColors];

    uint32_t cycleTime = 50 + (8 * (uint32_t)(255 - SEGMENT.speed));
    uint32_t it        = strip.now / cycleTime;
    if (it != SEGENV.step) {
        if (SEGMENT.speed > 0) SEGENV.aux0++;
        if (SEGENV.aux0 >= numColors) SEGENV.aux0 = 0;
        SEGENV.step = it;
    }

    for (unsigned i = 0; i < SEGLEN; i += numColors) {
        for (unsigned j = 0; j < numColors; j++) SEGMENT.setPixelColor(i + j, cols[SEGENV.aux0 + j]);
    }

    return FRAMETIME;
}

/*
 * Emulates a traffic light.
 */
uint16_t mode_traffic_light(void) {
    if (SEGLEN == 1) return mode_static();
    for (int i = 0; i < SEGLEN; i++)
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1));
    uint32_t mdelay = 500;
    for (int i = 0; i < SEGLEN - 2; i += 3) {
        switch (SEGENV.aux0) {
            case 0:
                SEGMENT.setPixelColor(i, 0x00FF0000);
                mdelay = 150 + (100 * (uint32_t)(255 - SEGMENT.speed));
                break;
            case 1:
                SEGMENT.setPixelColor(i, 0x00FF0000);
                mdelay = 150 + (20 * (uint32_t)(255 - SEGMENT.speed));
                SEGMENT.setPixelColor(i + 1, 0x00EECC00);
                break;
            case 2:
                SEGMENT.setPixelColor(i + 2, 0x0000FF00);
                mdelay = 150 + (100 * (uint32_t)(255 - SEGMENT.speed));
                break;
            case 3:
                SEGMENT.setPixelColor(i + 1, 0x00EECC00);
                mdelay = 150 + (20 * (uint32_t)(255 - SEGMENT.speed));
                break;
        }
    }

    if (strip.now - SEGENV.step > mdelay) {
        SEGENV.aux0++;
        if (SEGENV.aux0 == 1 && SEGMENT.intensity > 140) SEGENV.aux0 = 2;  //skip Red + Amber, to get US-style sequence
        if (SEGENV.aux0 > 3) SEGENV.aux0 = 0;
        SEGENV.step = strip.now;
    }

    return FRAMETIME;
}

/*
 * Sec flashes running on prim.
 */
#define FLASH_COUNT 4
uint16_t mode_chase_flash(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned flash_step = SEGENV.call % ((FLASH_COUNT * 2) + 1);

    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
    }

    unsigned delay = 10 + ((30 * (uint16_t)(255 - SEGMENT.speed)) / SEGLEN);
    if (flash_step < (FLASH_COUNT * 2)) {
        if (flash_step % 2 == 0) {
            unsigned n = SEGENV.step;
            unsigned m = (SEGENV.step + 1) % SEGLEN;
            SEGMENT.setPixelColor(n, SEGCOLOR(1));
            SEGMENT.setPixelColor(m, SEGCOLOR(1));
            delay = 20;
        } else {
            delay = 30;
        }
    } else {
        SEGENV.step = (SEGENV.step + 1) % SEGLEN;
    }
    return delay;
}

/*
 * Prim flashes running, followed by random color.
 */
uint16_t mode_chase_flash_random(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned flash_step = SEGENV.call % ((FLASH_COUNT * 2) + 1);

    for (int i = 0; i < SEGENV.aux1; i++) {
        SEGMENT.setPixelColor(i, SEGMENT.color_wheel(SEGENV.aux0));
    }

    unsigned delay = 1 + ((10 * (uint16_t)(255 - SEGMENT.speed)) / SEGLEN);
    if (flash_step < (FLASH_COUNT * 2)) {
        unsigned n = SEGENV.aux1;
        unsigned m = (SEGENV.aux1 + 1) % SEGLEN;
        if (flash_step % 2 == 0) {
            SEGMENT.setPixelColor(n, SEGCOLOR(0));
            SEGMENT.setPixelColor(m, SEGCOLOR(0));
            delay = 20;
        } else {
            SEGMENT.setPixelColor(n, SEGMENT.color_wheel(SEGENV.aux0));
            SEGMENT.setPixelColor(m, SEGCOLOR(1));
            delay = 30;
        }
    } else {
        SEGENV.aux1 = (SEGENV.aux1 + 1) % SEGLEN;

        if (SEGENV.aux1 == 0) {
            SEGENV.aux0 = get_random_wheel_index(SEGENV.aux0);
        }
    }
    return delay;
}
#undef FLASH_COUNT

/*
 * Alternating pixels running function.
 */
static uint16_t running(uint32_t color1, uint32_t color2, bool theatre = false) {
    int      width      = (theatre ? 3 : 1) + (SEGMENT.intensity >> 4);  // window
    uint32_t cycleTime  = 50 + (255 - SEGMENT.speed);
    uint32_t it         = strip.now / cycleTime;
    bool     usePalette = color1 == SEGCOLOR(0);

    for (int i = 0; i < SEGLEN; i++) {
        uint32_t col = color2;
        if (usePalette) color1 = SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0);
        if (theatre) {
            if ((i % width) == SEGENV.aux0) col = color1;
        } else {
            int pos = (i % (width << 1));
            if ((pos < SEGENV.aux0 - width) || ((pos >= SEGENV.aux0) && (pos < SEGENV.aux0 + width))) col = color1;
        }
        SEGMENT.setPixelColor(i, col);
    }

    if (it != SEGENV.step) {
        SEGENV.aux0 = (SEGENV.aux0 + 1) % (theatre ? width : (width << 1));
        SEGENV.step = it;
    }
    return FRAMETIME;
}

/*
 * Alternating color/sec pixels running.
 */
uint16_t mode_running_color(void) {
    return running(SEGCOLOR(0), SEGCOLOR(1));
}

/*
 * Random colored pixels running. ("Stream")
 */
uint16_t mode_running_random(void) {
    uint32_t cycleTime = 25 + (3 * (uint32_t)(255 - SEGMENT.speed));
    uint32_t it        = strip.now / cycleTime;
    if (SEGENV.call == 0) SEGENV.aux0 = random16();  // random seed for PRNG on start

    unsigned zoneSize = ((255 - SEGMENT.intensity) >> 4) + 1;
    uint16_t PRNG16   = SEGENV.aux0;

    unsigned z     = it % zoneSize;
    bool     nzone = (!z && it != SEGENV.aux1);
    for (int i = SEGLEN - 1; i >= 0; i--) {
        if (nzone || z >= zoneSize) {
            unsigned lastrand = PRNG16 >> 8;
            int16_t  diff     = 0;
            while (abs(diff) < 42) {  // make sure the difference between adjacent colors is big enough
                PRNG16 = (uint16_t)(PRNG16 * 2053) + 13849;  // next zone, next 'random' number
                diff   = (PRNG16 >> 8) - lastrand;
            }
            if (nzone) {
                SEGENV.aux0 = PRNG16;  // save next starting seed
                nzone       = false;
            }
            z = 0;
        }
        SEGMENT.setPixelColor(i, SEGMENT.color_wheel(PRNG16 >> 8));
        z++;
    }

    SEGENV.aux1 = it;
    return FRAMETIME;
}

/*
 * K.I.T.T.
 */
uint16_t mode_larson_scanner(void) {
    if (SEGLEN == 1) return mode_static();

    const unsigned speed  = FRAMETIME * map(SEGMENT.speed, 0, 255, 96, 2);  // map into useful range
    const unsigned pixels = SEGLEN / speed;                                 // how many pixels to advance per frame

    SEGMENT.fade_out(255 - SEGMENT.intensity);

    if (SEGENV.step > strip.now) return FRAMETIME;  // we have a pause

    unsigned index = SEGENV.aux1 + pixels;
    // are we slow enough to use frames per pixel?
    if (pixels == 0) {
        const unsigned frames = speed / SEGLEN;  // how many frames per 1 pixel
        if (SEGENV.step++ < frames) return FRAMETIME;
        SEGENV.step = 0;
        index++;
    }

    if (index > SEGLEN) {
        SEGENV.aux0 = !SEGENV.aux0;  // change direction
        SEGENV.aux1 = 0;             // reset position
        // set delay
        if (SEGENV.aux0 || SEGMENT.check2)
            SEGENV.step = strip.now + SEGMENT.custom1 * 25;  // multiply by 25ms
        else
            SEGENV.step = 0;

    } else {
        // paint as many pixels as needed
        for (unsigned i = SEGENV.aux1; i < index; i++) {
            unsigned j = (SEGENV.aux0) ? i : SEGLEN - 1 - i;
            uint32_t c = SEGMENT.color_from_palette(j, true, PALETTE_SOLID_WRAP, 0);
            SEGMENT.setPixelColor(j, c);
            if (SEGMENT.check1) {
                SEGMENT.setPixelColor(SEGLEN - 1 - j, SEGCOLOR(2) ? SEGCOLOR(2) : c);
            }
        }
        SEGENV.aux1 = index;
    }
    return FRAMETIME;
}

/*
 * Creates two Larson scanners moving in opposite directions
 * Custom mode by Keith Lord: https://github.com/kitesurfer1404/WS2812FX/blob/master/src/custom/DualLarson.h
 */
uint16_t mode_dual_larson_scanner(void) {
    SEGMENT.check1 = true;
    return mode_larson_scanner();
}

/*
 * Firing comets from one end. "Lighthouse"
 */
uint16_t mode_comet(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned counter = (strip.now * ((SEGMENT.speed >> 2) + 1)) & 0xFFFF;
    unsigned index   = (counter * SEGLEN) >> 16;
    if (SEGENV.call == 0) SEGENV.aux0 = index;

    SEGMENT.fade_out(SEGMENT.intensity);

    SEGMENT.setPixelColor(index, SEGMENT.color_from_palette(index, true, PALETTE_SOLID_WRAP, 0));
    if (index > SEGENV.aux0) {
        for (unsigned i = SEGENV.aux0; i < index; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }
    } else if (index < SEGENV.aux0 && index < 10) {
        for (unsigned i = 0; i < index; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }
    }
    SEGENV.aux0 = index++;

    return FRAMETIME;
}

/*
 * Fireworks function.
 */
// DW: WLED's mode_fireworks(), the base of mode_rain(); a local copy (another batch registers Fireworks).
static uint16_t fireworks(void) {
    if (SEGLEN == 1) return mode_static();
    const uint16_t width  = SEGMENT.is2D() ? SEGMENT.virtualWidth() : SEGMENT.virtualLength();
    const uint16_t height = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGENV.aux0 = UINT16_MAX;
        SEGENV.aux1 = UINT16_MAX;
    }
    SEGMENT.fade_out(128);

    uint8_t x = SEGENV.aux0 % width, y = SEGENV.aux0 / width;  // 2D coordinates stored in upper and lower byte
    if (!SEGENV.step) {
        // fireworks mode (blur flares)
        bool     valid1 = (SEGENV.aux0 < width * height);
        bool     valid2 = (SEGENV.aux1 < width * height);
        uint32_t sv1 = 0, sv2 = 0;
        if (valid1) sv1 = SEGMENT.is2D() ? SEGMENT.getPixelColorXY(x, y) : SEGMENT.getPixelColor(SEGENV.aux0);  // get spark color
        if (valid2) sv2 = SEGMENT.is2D() ? SEGMENT.getPixelColorXY(x, y) : SEGMENT.getPixelColor(SEGENV.aux1);
        SEGMENT.blur(16);  // used in mode_rain()
        if (valid1) {
            if (SEGMENT.is2D())
                SEGMENT.setPixelColorXY(x, y, sv1);
            else
                SEGMENT.setPixelColor(SEGENV.aux0, sv1);
        }  // restore spark color after blur
        if (valid2) {
            if (SEGMENT.is2D())
                SEGMENT.setPixelColorXY(x, y, sv2);
            else
                SEGMENT.setPixelColor(SEGENV.aux1, sv2);
        }  // restore old spark color after blur
    }

    for (int i = 0; i < max(1, width / 20); i++) {
        if (random8(129 - (SEGMENT.intensity >> 1)) == 0) {
            uint16_t index = random16(width * height);
            x              = index % width;
            y              = index / width;
            uint32_t col   = SEGMENT.color_from_palette(random8(), false, false, 0);
            if (SEGMENT.is2D())
                SEGMENT.setPixelColorXY(x, y, col);
            else
                SEGMENT.setPixelColor(index, col);
            SEGENV.aux1 = SEGENV.aux0;  // old spark
            SEGENV.aux0 = index;        // remember where spark occurred
        }
    }
    return FRAMETIME;
}

//Twinkling LEDs running. Inspired by https://github.com/kitesurfer1404/WS2812FX/blob/master/src/custom/Rain.h
uint16_t mode_rain(void) {
    if (SEGLEN == 1) return mode_static();
    const unsigned width  = SEGMENT.virtualWidth();
    const unsigned height = SEGMENT.virtualHeight();
    SEGENV.step += FRAMETIME;
    if (SEGENV.call && SEGENV.step > (5U + (50U * (255U - SEGMENT.speed)) / SEGLEN)) {  // SPEED_FORMULA_L
        SEGENV.step = 1;
        if (SEGMENT.is2D()) {
            //uint32_t ctemp[width];
            //for (int i = 0; i<width; i++) ctemp[i] = SEGMENT.getPixelColorXY(i, height-1);
            SEGMENT.move(6, 1, true);  // move all pixels down
            //for (int i = 0; i<width; i++) SEGMENT.setPixelColorXY(i, 0, ctemp[i]); // wrap around
            SEGENV.aux0 = (SEGENV.aux0 % width) + (SEGENV.aux0 / width + 1) * width;
            SEGENV.aux1 = (SEGENV.aux1 % width) + (SEGENV.aux1 / width + 1) * width;
        } else {
            //shift all leds left
            uint32_t ctemp = SEGMENT.getPixelColor(0);
            for (int i = 0; i < SEGLEN - 1; i++) {
                SEGMENT.setPixelColor(i, SEGMENT.getPixelColor(i + 1));
            }
            SEGMENT.setPixelColor(SEGLEN - 1, ctemp);  // wrap around
            SEGENV.aux0++;                             // increase spark index
            SEGENV.aux1++;
        }
        if (SEGENV.aux0 == 0) SEGENV.aux0 = UINT16_MAX;  // reset previous spark position
        if (SEGENV.aux1 == 0) SEGENV.aux0 = UINT16_MAX;  // reset previous spark position
        if (SEGENV.aux0 >= width * height) SEGENV.aux0 = 0;  // ignore
        if (SEGENV.aux1 >= width * height) SEGENV.aux1 = 0;
    }
    return fireworks();
}

/*
 * Tetris or Stacking (falling bricks) Effect
 * by Blaz Kristan (AKA blazoncek) (https://github.com/blazoncek, https://blaz.at/home)
 */
namespace {
//20 bytes
typedef struct Tetris {
    float    pos;
    float    speed;
    uint8_t  col;    // color index
    uint16_t brick;  // brick size in pixels
    uint16_t stack;  // stack size in pixels
    uint32_t step;   // 2D-fication of SEGENV.step (state)
} tetris;
}  // namespace

uint16_t mode_tetrix(void) {
    if (SEGLEN == 1) return mode_static();
    unsigned strips   = SEGMENT.nrOfVStrips();  // allow running on virtual strips (columns in 2D segment)
    unsigned dataSize = sizeof(tetris);
    if (!SEGENV.allocateData(dataSize * strips)) return mode_static();  //allocation failed
    Tetris* drops = reinterpret_cast<Tetris*>(SEGENV.data);

    //if (SEGENV.call == 0) SEGMENT.fill(SEGCOLOR(1));  // will fill entire segment (1D or 2D), then use drop->step = 0 below

    // virtualStrip idea by @ewowi (Ewoud Wijma)
    // requires virtual strip # to be embedded into upper 16 bits of index in setPixelcolor()
    // the following functions will not work on virtual strips: fill(), fade_out(), fadeToBlack(), blur()
    struct virtualStrip {
        static void runStrip(size_t stripNr, Tetris* drop) {
            // initialize dropping on first call or segment full
            if (SEGENV.call == 0) {
                drop->stack = 0;                    // reset brick stack size
                drop->step  = strip.now + 2000;     // start by fading out strip
                if (SEGMENT.check1) drop->col = 0;  // use only one color from palette
            }

            if (drop->step == 0) {  // init brick
                // speed calculation: a single brick should reach bottom of strip in X seconds
                // if the speed is set to 1 this should take 5s and at 255 it should take 0.25s
                // as this is dependant on SEGLEN it should be taken into account and the fact that effect runs every FRAMETIME s
                int speed   = SEGMENT.speed ? SEGMENT.speed : random8(1, 255);
                speed       = map(speed, 1, 255, 5000, 250);                   // time taken for full (SEGLEN) drop
                drop->speed = float(SEGLEN * FRAMETIME) / float(speed);        // set speed
                drop->pos   = SEGLEN;                                          // start at end of segment (no need to subtract 1)
                if (!SEGMENT.check1) drop->col = random8(0, 15) << 4;          // limit color choices so there is enough HUE gap
                drop->step  = 1;                                               // drop state (0 init, 1 forming, 2 falling)
                drop->brick = (SEGMENT.intensity ? (SEGMENT.intensity >> 5) + 1 : random8(1, 5)) * (1 + (SEGLEN >> 6));  // size of brick
            }

            if (drop->step == 1) {  // forming
                if (random8() >> 6) {  // random drop
                    drop->step = 2;    // fall
                }
            }

            if (drop->step == 2) {                // falling
                if (drop->pos > drop->stack) {    // fall until top of stack
                    drop->pos -= drop->speed;     // may add gravity as: speed += gravity
                    if (int(drop->pos) < int(drop->stack)) drop->pos = drop->stack;
                    for (int i = int(drop->pos); i < SEGLEN; i++) {
                        uint32_t col = i < int(drop->pos) + drop->brick ? SEGMENT.color_from_palette(drop->col, false, false, 0) : SEGCOLOR(1);
                        SEGMENT.setPixelColor(indexToVStrip(i, stripNr), col);
                    }
                } else {                        // we hit bottom
                    drop->step = 0;             // proceed with next brick, go back to init
                    drop->stack += drop->brick; // increase the stack size
                    if (drop->stack >= SEGLEN) drop->step = strip.now + 2000;  // fade out stack
                }
            }

            if (drop->step > 2) {  // fade strip
                drop->brick = 0;   // reset brick size (no more growing)
                if (drop->step > strip.now) {
                    // allow fading of virtual strip
                    for (int i = 0; i < SEGLEN; i++) SEGMENT.blendPixelColor(indexToVStrip(i, stripNr), SEGCOLOR(1), 25);  // 10% blend
                } else {
                    drop->stack = 0;                      // reset brick stack size
                    drop->step  = 0;                      // proceed with next brick
                    if (SEGMENT.check1) drop->col += 8;   // gradually increase palette index
                }
            }
        }
    };

    for (unsigned stripNr = 0; stripNr < strips; stripNr++)
        virtualStrip::runStrip(stripNr, &drops[stripNr]);

    return FRAMETIME;
}

}  // namespace wled
