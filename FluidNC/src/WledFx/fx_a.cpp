// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_a.ts; tools/fx_parity.py checks them.
//
// Batch a: wipes/sweeps, random colors, dynamic, fade, theater rainbow, saw,
// dissolve, sparkles, strobes, blink rainbow, android.
#include "WledFx.h"

namespace wled {

// ---- Blink / strobe helper ----------------------------------------------------
static uint16_t blink(uint32_t color1, uint32_t color2, bool strobe, bool do_palette) {
    uint32_t cycleTime = (255 - SEGMENT.speed) * 20;
    uint32_t onTime    = FRAMETIME;
    if (!strobe) onTime += ((cycleTime * SEGMENT.intensity) >> 8);
    cycleTime += FRAMETIME * 2;
    uint32_t it  = strip.now / cycleTime;
    uint32_t rem = strip.now % cycleTime;

    bool on = false;
    if (it != SEGENV.step  // new iteration, force on state for one frame, even if set time is too brief
        || rem <= onTime) {
        on = true;
    }

    SEGENV.step = it;  // save previous iteration

    uint32_t color = on ? color1 : color2;
    if (color == color1 && do_palette) {
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }
    } else
        SEGMENT.fill(color);

    return FRAMETIME;
}

// Classic Blink effect. Cycling through the rainbow.
uint16_t mode_blink_rainbow(void) {
    return blink(SEGMENT.color_wheel(SEGENV.call & 0xFF), SEGCOLOR(1), false, false);
}

// Classic Strobe effect. Cycling through the rainbow.
uint16_t mode_strobe_rainbow(void) {
    return blink(SEGMENT.color_wheel(SEGENV.call & 0xFF), SEGCOLOR(1), true, false);
}

// ---- Color wipe helper ----------------------------------------------------------
// LEDs are turned on (color1) in sequence, then turned off (color2) in sequence.
// if (bool rev == true) then LEDs are turned off in reverse order
static uint16_t color_wipe(bool rev, bool useRandomColors) {
    if (SEGLEN == 1) return mode_static();
    uint32_t cycleTime = 750 + (255 - SEGMENT.speed) * 150;
    uint32_t perc      = strip.now % cycleTime;
    unsigned prog      = (perc * 65535) / cycleTime;
    bool     back      = (prog > 32767);
    if (back) {
        prog -= 32767;
        if (SEGENV.step == 0) SEGENV.step = 1;
    } else {
        if (SEGENV.step == 2) SEGENV.step = 3;  // trigger color change
    }

    if (useRandomColors) {
        if (SEGENV.call == 0) {
            SEGENV.aux0 = random8();
            SEGENV.step = 3;
        }
        if (SEGENV.step == 1) {  // if flag set, change to new random color
            SEGENV.aux1 = get_random_wheel_index(SEGENV.aux0);
            SEGENV.step = 2;
        }
        if (SEGENV.step == 3) {
            SEGENV.aux0 = get_random_wheel_index(SEGENV.aux1);
            SEGENV.step = 0;
        }
    }

    unsigned ledIndex = (prog * SEGLEN) >> 15;
    uint16_t rem      = (prog * SEGLEN) * 2;  // mod 0xFFFF by truncating
    rem /= (SEGMENT.intensity + 1);
    if (rem > 255) rem = 255;

    uint32_t col1 = useRandomColors ? SEGMENT.color_wheel(SEGENV.aux1) : SEGCOLOR(1);
    for (unsigned i = 0; i < SEGLEN; i++) {
        unsigned index = (rev && back) ? SEGLEN - 1 - i : i;
        uint32_t col0  = useRandomColors ? SEGMENT.color_wheel(SEGENV.aux0) : SEGMENT.color_from_palette(index, true, PALETTE_SOLID_WRAP, 0);

        if (i < ledIndex) {
            SEGMENT.setPixelColor(index, back ? col1 : col0);
        } else {
            SEGMENT.setPixelColor(index, back ? col0 : col1);
            if (i == ledIndex) SEGMENT.setPixelColor(index, color_blend(back ? col0 : col1, back ? col1 : col0, rem));
        }
    }
    return FRAMETIME;
}

// Lights all LEDs one after another. Turns off opposite
uint16_t mode_color_sweep(void) {
    return color_wipe(true, false);
}

// Turns all LEDs after each other to a random color. Then starts over with another color.
uint16_t mode_color_wipe_random(void) {
    return color_wipe(false, true);
}

// Random color introduced alternating from start and end of strip.
uint16_t mode_color_sweep_random(void) {
    return color_wipe(true, true);
}

// ---- Random Colors ---------------------------------------------------------------
uint16_t mode_random_color(void) {
    uint32_t cycleTime = 200 + (255 - SEGMENT.speed) * 50;
    uint32_t it        = strip.now / cycleTime;
    uint32_t rem       = strip.now % cycleTime;
    unsigned fadedur   = (cycleTime * SEGMENT.intensity) >> 8;

    uint32_t fade = 255;
    if (fadedur) {
        fade = (rem * 255) / fadedur;
        if (fade > 255) fade = 255;
    }

    if (SEGENV.call == 0) {
        SEGENV.aux0 = random8();
        SEGENV.step = 2;
    }
    if (it != SEGENV.step)  // new color
    {
        SEGENV.aux1 = SEGENV.aux0;
        SEGENV.aux0 = get_random_wheel_index(SEGENV.aux0);  // aux0 will store our random color wheel index
        SEGENV.step = it;
    }

    SEGMENT.fill(color_blend(SEGMENT.color_wheel(SEGENV.aux1), SEGMENT.color_wheel(SEGENV.aux0), fade));
    return FRAMETIME;
}

// ---- Dynamic ---------------------------------------------------------------------
uint16_t mode_dynamic(void) {
    if (!SEGENV.allocateData(SEGLEN)) return mode_static();  // allocation failed

    if (SEGENV.call == 0) {
        // SEGMENT.fill(BLACK);
        for (int i = 0; i < SEGLEN; i++) SEGENV.data[i] = random8();
    }

    uint32_t cycleTime = 50 + (255 - SEGMENT.speed) * 15;
    uint32_t it        = strip.now / cycleTime;
    if (it != SEGENV.step && SEGMENT.speed != 0)  // new color
    {
        for (int i = 0; i < SEGLEN; i++) {
            if (random8() <= SEGMENT.intensity) SEGENV.data[i] = random8();  // random color index
        }
        SEGENV.step = it;
    }

    if (SEGMENT.check1) {
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.blendPixelColor(i, SEGMENT.color_wheel(SEGENV.data[i]), 16);
        }
    } else {
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_wheel(SEGENV.data[i]));
        }
    }
    return FRAMETIME;
}

// effect "Dynamic" with smooth color-fading
uint16_t mode_dynamic_smooth(void) {
    bool old       = SEGMENT.check1;
    SEGMENT.check1 = true;
    mode_dynamic();
    SEGMENT.check1 = old;
    return FRAMETIME;
}

// ---- Fade ------------------------------------------------------------------------
uint16_t mode_fade(void) {
    unsigned counter = (strip.now * ((SEGMENT.speed >> 3) + 10));
    unsigned lum     = triwave16(counter) >> 8;

    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0), lum));
    }

    return FRAMETIME;
}

// ---- Running / theater helper ------------------------------------------------------
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

// Theatre-style crawling lights with rainbow effect.
uint16_t mode_theater_chase_rainbow(void) {
    return running(SEGMENT.color_wheel(SEGENV.step), SEGCOLOR(1), true);
}

// ---- Running lights helper (saw only: the dual / sin_gap path is not in this batch)
static uint16_t running_base(bool saw) {
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
        uint8_t  s  = sin8_t(a);
        uint32_t ca = color_blend(SEGCOLOR(1), SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0), s);
        SEGMENT.setPixelColor(i, ca);
    }

    return FRAMETIME;
}

// Running lights effect with sawtooth transition.
uint16_t mode_saw(void) {
    return running_base(true);
}

// ---- Dissolve helper -------------------------------------------------------------
static uint16_t dissolve(uint32_t color) {
    unsigned dataSize = sizeof(uint32_t) * SEGLEN;
    if (!SEGENV.allocateData(dataSize)) return mode_static();  // allocation failed
    uint32_t* pixels = reinterpret_cast<uint32_t*>(SEGENV.data);

    if (SEGENV.call == 0) {
        for (unsigned i = 0; i < SEGLEN; i++) pixels[i] = SEGCOLOR(1);
        SEGENV.aux0 = 1;
    }

    for (int j = 0; j <= SEGLEN / 15; j++) {
        if (random8() <= SEGMENT.intensity) {
            for (size_t times = 0; times < 10; times++) {  // attempt to spawn a new pixel 10 times
                unsigned i = random16(SEGLEN);
                if (SEGENV.aux0) {  // dissolve to primary/palette
                    if (pixels[i] == SEGCOLOR(1)) {
                        pixels[i] = color == SEGCOLOR(0) ? SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0) : color;
                        break;  // only spawn 1 new pixel per frame per 50 LEDs
                    }
                } else {  // dissolve to secondary
                    if (pixels[i] != SEGCOLOR(1)) {
                        pixels[i] = SEGCOLOR(1);
                        break;
                    }
                }
            }
        }
    }
    // fix for #4401
    for (unsigned i = 0; i < SEGLEN; i++) SEGMENT.setPixelColor(i, pixels[i]);

    if (SEGENV.step > (255 - SEGMENT.speed) + 15U) {
        SEGENV.aux0 = !SEGENV.aux0;
        SEGENV.step = 0;
    } else {
        SEGENV.step++;
    }

    return FRAMETIME;
}

// Blink several LEDs on and then off in random colors
uint16_t mode_dissolve_random(void) {
    return dissolve(SEGMENT.color_wheel(random8()));
}

// ---- Sparkle Dark / Sparkle+ ------------------------------------------------------
// Lights all LEDs in the color. Flashes single col 1 pixels randomly. (List name: Sparkle Dark)
uint16_t mode_flash_sparkle(void) {
    if (!SEGMENT.check2)
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }

    if (strip.now - SEGENV.aux0 > SEGENV.step) {
        if (random8((255 - SEGMENT.intensity) >> 4) == 0) {
            SEGMENT.setPixelColor(random16(SEGLEN), SEGCOLOR(1));  // flash
        }
        SEGENV.step = strip.now;
        SEGENV.aux0 = 255 - SEGMENT.speed;
    }
    return FRAMETIME;
}

// Like flash sparkle. With more flash.
uint16_t mode_hyper_sparkle(void) {
    if (!SEGMENT.check2)
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }

    if (strip.now - SEGENV.aux0 > SEGENV.step) {
        if (random8((255 - SEGMENT.intensity) >> 4) == 0) {
            for (int i = 0; i < max(1, SEGLEN / 3); i++) {
                SEGMENT.setPixelColor(random16(SEGLEN), SEGCOLOR(1));
            }
        }
        SEGENV.step = strip.now;
        SEGENV.aux0 = 255 - SEGMENT.speed;
    }
    return FRAMETIME;
}

// ---- Strobe Mega -----------------------------------------------------------------
// Strobe effect with different strobe count and pause, controlled by speed.
uint16_t mode_multi_strobe(void) {
    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1));
    }

    SEGENV.aux0    = 50 + 20 * (uint16_t)(255 - SEGMENT.speed);
    unsigned count = 2 * ((SEGMENT.intensity / 10) + 1);
    if (SEGENV.aux1 < count) {
        if ((SEGENV.aux1 & 1) == 0) {
            SEGMENT.fill(SEGCOLOR(0));
            SEGENV.aux0 = 15;
        } else {
            SEGENV.aux0 = 50;
        }
    }

    if (strip.now - SEGENV.aux0 > SEGENV.step) {
        SEGENV.aux1++;
        if (SEGENV.aux1 > count) SEGENV.aux1 = 0;
        SEGENV.step = strip.now;
    }

    return FRAMETIME;
}

// ---- Android loading circle --------------------------------------------------------
uint16_t mode_android(void) {
    for (int i = 0; i < SEGLEN; i++) {
        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 1));
    }

    if (SEGENV.aux1 > (SEGMENT.intensity * SEGLEN) / 255) {
        SEGENV.aux0 = 1;
    } else {
        if (SEGENV.aux1 < 2) SEGENV.aux0 = 0;
    }

    unsigned a = SEGENV.step & 0xFFFFU;

    if (SEGENV.aux0 == 0) {
        if (SEGENV.call % 3 == 1) {
            a++;
        } else {
            SEGENV.aux1++;
        }
    } else {
        a++;
        if (SEGENV.call % 3 != 1) SEGENV.aux1--;
    }

    if (a >= SEGLEN) a = 0;

    if (a + SEGENV.aux1 < SEGLEN) {
        for (unsigned i = a; i < a + SEGENV.aux1; i++) {
            SEGMENT.setPixelColor(i, SEGCOLOR(0));
        }
    } else {
        for (unsigned i = a; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGCOLOR(0));
        }
        for (unsigned i = 0; i < SEGENV.aux1 - (SEGLEN - a); i++) {
            SEGMENT.setPixelColor(i, SEGCOLOR(0));
        }
    }
    SEGENV.step = a;

    return 3 + ((8 * (uint32_t)(255 - SEGMENT.speed)) / SEGLEN);
}

}  // namespace wled
