// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_ref.ts; tools/fx_parity.py checks them.
//
// Reference batch: the patterns every other fx_*.cpp follows.
#include "WledFx.h"

namespace wled {

// ---- Blink ------------------------------------------------------------------
static uint16_t blink(uint32_t color1, uint32_t color2, bool strobe, bool do_palette) {
    uint32_t cycleTime = (255 - SEGMENT.speed) * 20;
    uint32_t onTime    = FRAMETIME;
    if (!strobe) onTime += ((cycleTime * SEGMENT.intensity) >> 8);
    cycleTime += FRAMETIME * 2;
    uint32_t it  = strip.now / cycleTime;
    uint32_t rem = strip.now % cycleTime;

    bool on = false;
    if (it != SEGENV.step || rem <= onTime) {
        on = true;
    }
    SEGENV.step = it;

    uint32_t color = on ? color1 : color2;
    if (color == color1 && do_palette) {
        for (int i = 0; i < SEGLEN; i++) {
            SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
        }
    } else
        SEGMENT.fill(color);

    return FRAMETIME;
}

uint16_t mode_blink(void) {
    return blink(SEGCOLOR(0), SEGCOLOR(1), false, true);
}

// ---- Noise 1 ------------------------------------------------------------------
uint16_t mode_noise16_1(void) {
    unsigned scale = 320;
    SEGENV.step += (1 + SEGMENT.speed / 16);

    for (int i = 0; i < SEGLEN; i++) {
        unsigned shift_x = beatsin8_t(11);
        unsigned shift_y = SEGENV.step / 42;
        unsigned real_x  = (i + shift_x) * scale;
        unsigned real_y  = (i + shift_y) * scale;
        uint32_t real_z  = SEGENV.step;
        unsigned noise   = inoise16(real_x, real_y, real_z) >> 8;
        unsigned index   = sin8_t(noise * 3);

        SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(index, false, PALETTE_SOLID_WRAP, 0));
    }

    return FRAMETIME;
}

// ---- Polar Lights (2D) -----------------------------------------------------------
uint16_t mode_2DPolarLights(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    CRGBPalette16 auroraPalette = { 0x000000, 0x003300, 0x006600, 0x009900, 0x00cc00, 0x00ff00, 0x33ff00, 0x66ff00,
                                    0x99ff00, 0xccff00, 0xffff00, 0xffcc00, 0xff9900, 0xff6600, 0xff3300, 0xff0000 };

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
        SEGENV.step = 0;
    }

    float    adjustHeight = (float)map(rows, 8, 32, 28, 12);
    unsigned adjScale     = map(cols, 8, 64, 310, 63);
    unsigned _scale       = map(SEGMENT.intensity, 0, 255, 30, adjScale);
    int      _speed       = map(SEGMENT.speed, 0, 255, 128, 16);

    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            SEGENV.step++;
            SEGMENT.setPixelColorXY(
                x, y,
                ColorFromPalette(auroraPalette,
                                 qsub8(inoise8((SEGENV.step % 2) + x * _scale, y * 16 + SEGENV.step % 16, SEGENV.step / _speed),
                                       fabsf((float)rows / 2.0f - (float)y) * adjustHeight)));
        }
    }

    return FRAMETIME;
}

}  // namespace wled
