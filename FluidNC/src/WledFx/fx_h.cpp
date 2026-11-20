// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_h.ts; tools/fx_parity.py checks them.
//
// Batch h: WLED's 2D matrix effects, drawn on the runner's 16 x 16 matrix.
#include "WledFx.h"

#include <alloca.h>

namespace wled {

#define XY(x, y) SEGMENT.XY(x, y)

// WLED's FX.h: its effects' fixed 42 FPS slot (the layer's FRAMETIME is our frame rate)
static constexpr uint32_t FRAMETIME_FIXED = 1000 / 42;

// WLED's util.cpp
static uint16_t crc16(const unsigned char* data_p, size_t length) {
    uint8_t  x;
    uint16_t crc = 0xFFFF;
    if (!length) return 0x1D0F;
    while (length--) {
        x = crc >> 8 ^ *data_p++;
        x ^= x >> 4;
        crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x);
    }
    return crc;
}

// ---- Black Hole ---------------------------------------------------------------
uint16_t mode_2DBlackHole(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();
    int       x, y;

    SEGMENT.fadeToBlackBy(16 + (SEGMENT.speed >> 3));
    unsigned long t = strip.now / 128;
    // outer stars
    for (size_t i = 0; i < 8; i++) {
        x = beatsin8_t(SEGMENT.custom1 >> 3, 0, cols - 1, 0, ((i % 2) ? 128 : 0) + t * i);
        y = beatsin8_t(SEGMENT.intensity >> 3, 0, rows - 1, 0, ((i % 2) ? 192 : 64) + t * i);
        SEGMENT.addPixelColorXY(x, y, SEGMENT.color_from_palette(i * 32, false, PALETTE_SOLID_WRAP, SEGMENT.check1 ? 0 : 255));
    }
    // inner stars
    for (size_t i = 0; i < 4; i++) {
        x = beatsin8_t(SEGMENT.custom2 >> 3, cols / 4, cols - 1 - cols / 4, 0, ((i % 2) ? 128 : 0) + t * i);
        y = beatsin8_t(SEGMENT.custom3, rows / 4, rows - 1 - rows / 4, 0, ((i % 2) ? 192 : 64) + t * i);
        SEGMENT.addPixelColorXY(x, y, SEGMENT.color_from_palette(255 - i * 64, false, PALETTE_SOLID_WRAP, SEGMENT.check1 ? 0 : 255));
    }
    // central white dot
    SEGMENT.setPixelColorXY(cols / 2, rows / 2, WHITE);
    // blur everything a bit
    if (SEGMENT.check3) SEGMENT.blur(16, cols * rows < 100);

    return FRAMETIME;
}

// ---- Colored Bursts -------------------------------------------------------------
uint16_t mode_2DColoredBursts(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGENV.aux0 = 0;  // start with red hue
    }

    bool dot  = SEGMENT.check3;
    bool grad = SEGMENT.check1;

    byte numLines = SEGMENT.intensity / 16 + 1;

    SEGENV.aux0++;  // hue
    SEGMENT.fadeToBlackBy(40);
    for (size_t i = 0; i < numLines; i++) {
        byte x1    = beatsin8_t(2 + SEGMENT.speed / 16, 0, (cols - 1));
        byte x2    = beatsin8_t(1 + SEGMENT.speed / 16, 0, (rows - 1));
        byte y1    = beatsin8_t(5 + SEGMENT.speed / 16, 0, (cols - 1), 0, i * 24);
        byte y2    = beatsin8_t(3 + SEGMENT.speed / 16, 0, (rows - 1), 0, i * 48 + 64);
        CRGB color = ColorFromPalette(SEGPALETTE, i * 255 / numLines + (SEGENV.aux0 & 0xFF), 255, LINEARBLEND);

        byte xsteps = abs8(x1 - y1) + 1;
        byte ysteps = abs8(x2 - y2) + 1;
        byte steps  = xsteps >= ysteps ? xsteps : ysteps;
        // Draw gradient line
        for (size_t j = 1; j <= steps; j++) {
            uint8_t rate = j * 255 / steps;
            byte    dx   = lerp8by8(x1, y1, rate);
            byte    dy   = lerp8by8(x2, y2, rate);
            SEGMENT.addPixelColorXY(dx, dy, color);  // use setPixelColorXY for different look
            if (grad) SEGMENT.fadePixelColorXY(dx, dy, rate);
        }

        if (dot) {  // add white point at the ends of line
            SEGMENT.setPixelColorXY(x1, x2, WHITE);
            SEGMENT.setPixelColorXY(y1, y2, DARKSLATEGRAY);
        }
    }
    if (SEGMENT.custom3) SEGMENT.blur(SEGMENT.custom3 / 2);

    return FRAMETIME;
}

// ---- DNA Spiral -------------------------------------------------------------------
uint16_t mode_2DDNASpiral(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
    }

    unsigned speeds = SEGMENT.speed / 2 + 7;
    unsigned freq   = SEGMENT.intensity / 8;

    uint32_t ms = strip.now / 20;
    SEGMENT.fadeToBlackBy(135);

    for (int i = 0; i < rows; i++) {
        int      x   = beatsin8_t(speeds, 0, cols - 1, 0, i * freq) + beatsin8_t(speeds - 7, 0, cols - 1, 0, i * freq + 128);
        int      x1  = beatsin8_t(speeds, 0, cols - 1, 0, 128 + i * freq) + beatsin8_t(speeds - 7, 0, cols - 1, 0, 128 + 64 + i * freq);
        unsigned hue = (i * 128 / rows) + ms;
        // skip every 4th row every now and then (fade it more)
        if ((i + ms / 8) & 3) {
            // draw a gradient line between x and x1
            x               = x / 2;
            x1              = x1 / 2;
            unsigned steps    = abs8(x - x1) + 1;
            bool     positive = (x1 >= x);  // direction of drawing
            for (size_t k = 1; k <= steps; k++) {
                unsigned rate = k * 255 / steps;
                unsigned dx   = positive ? (x + k - 1) : (x - k + 1);  // behaves the same as "lerp8by8" but does not create holes
                SEGMENT.addPixelColorXY(dx, i, ColorFromPalette(SEGPALETTE, hue, 255, LINEARBLEND));  // use setPixelColorXY for different look
                SEGMENT.fadePixelColorXY(dx, i, rate);
            }
            SEGMENT.setPixelColorXY(x, i, DARKSLATEGRAY);
            SEGMENT.setPixelColorXY(x1, i, WHITE);
        }
    }

    return FRAMETIME;
}

// ---- Frizzles -----------------------------------------------------------------------
uint16_t mode_2DFrizzles(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    SEGMENT.fadeToBlackBy(16);
    for (size_t i = 8; i > 0; i--) {
        SEGMENT.addPixelColorXY(beatsin8_t(SEGMENT.speed / 8 + i, 0, cols - 1), beatsin8_t(SEGMENT.intensity / 8 - i, 0, rows - 1),
                                ColorFromPalette(SEGPALETTE, beatsin8_t(12, 0, 255), 255, LINEARBLEND));
    }
    SEGMENT.blur(SEGMENT.custom1 >> 3);

    return FRAMETIME;
}

// ---- Game Of Life ---------------------------------------------------------------------
namespace {
typedef struct ColorCount {
    CRGB   color;
    int8_t count;
} colorCount;
}  // namespace

uint16_t mode_2Dgameoflife(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int      cols         = SEGMENT.virtualWidth();
    const int      rows         = SEGMENT.virtualHeight();
    const unsigned dataSize     = sizeof(CRGB) * SEGMENT.length();  // using width*height prevents reallocation if mirroring is enabled
    const int      crcBufferLen = 2;

    if (!SEGENV.allocateData(dataSize + sizeof(uint16_t) * crcBufferLen)) return mode_static();  // allocation failed
    CRGB*     prevLeds  = reinterpret_cast<CRGB*>(SEGENV.data);
    uint16_t* crcBuffer = reinterpret_cast<uint16_t*>(SEGENV.data + dataSize);

    CRGB backgroundColor = SEGCOLOR(1);

    if (SEGENV.call == 0 || strip.now - SEGMENT.step > 3000) {
        SEGENV.step = strip.now;
        SEGENV.aux0 = 0;

        // give the leds random state and colors (based on intensity, colors from palette or all posible colors are chosen)
        for (int x = 0; x < cols; x++)
            for (int y = 0; y < rows; y++) {
                unsigned state = random8() % 2;
                if (state == 0)
                    SEGMENT.setPixelColorXY(x, y, backgroundColor);
                else
                    SEGMENT.setPixelColorXY(x, y, SEGMENT.color_from_palette(random8(), false, PALETTE_SOLID_WRAP, 255));
            }

        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) prevLeds[XY(x, y)] = CRGB::Black;
        memset(crcBuffer, 0, sizeof(uint16_t) * crcBufferLen);
    } else if (strip.now - SEGENV.step < FRAMETIME_FIXED * (uint32_t)map(SEGMENT.speed, 0, 255, 64, 4)) {
        // update only when appropriate time passes (in 42 FPS slots)
        return FRAMETIME;
    }

    // copy previous leds (save previous generation)
    for (int x = 0; x < cols; x++)
        for (int y = 0; y < rows; y++) prevLeds[XY(x, y)] = SEGMENT.getPixelColorXY(x, y);

    // calculate new leds
    for (int x = 0; x < cols; x++)
        for (int y = 0; y < rows; y++) {
            colorCount colorsCount[9];  // count the different colors in the 3*3 matrix
            for (int i = 0; i < 9; i++) colorsCount[i] = { backgroundColor, 0 };  // init colorsCount

            // iterate through neighbors and count them and their different colors
            int neighbors = 0;
            for (int i = -1; i <= 1; i++)
                for (int j = -1; j <= 1; j++) {  // iterate through 3*3 matrix
                    if (i == 0 && j == 0) continue;  // ignore itself
                    // wrap around segment
                    int xx = x + i, yy = y + j;
                    if (x + i < 0)
                        xx = cols - 1;
                    else if (x + i >= cols)
                        xx = 0;
                    if (y + j < 0)
                        yy = rows - 1;
                    else if (y + j >= rows)
                        yy = 0;

                    unsigned xy = XY(xx, yy);  // previous cell xy to check
                    // count different neighbours and colors
                    if (prevLeds[xy] != backgroundColor) {
                        neighbors++;
                        bool colorFound = false;
                        int  k;
                        for (k = 0; k < 9 && colorsCount[k].count != 0; k++)
                            if (colorsCount[k].color == prevLeds[xy]) {
                                colorsCount[k].count++;
                                colorFound = true;
                            }
                        if (!colorFound) colorsCount[k] = { prevLeds[xy], 1 };  // add new color found in the array
                    }
                }  // i,j

            // Rules of Life
            uint32_t col = uint32_t(prevLeds[XY(x, y)]) & 0x00FFFFFF;
            uint32_t bgc = RGBW32(backgroundColor.r, backgroundColor.g, backgroundColor.b, 0);
            if ((col != bgc) && (neighbors < 2))
                SEGMENT.setPixelColorXY(x, y, bgc);  // Loneliness
            else if ((col != bgc) && (neighbors > 3))
                SEGMENT.setPixelColorXY(x, y, bgc);  // Overpopulation
            else if ((col == bgc) && (neighbors == 3)) {  // Reproduction
                // find dominant color and assign it to a cell
                colorCount dominantColorCount = { backgroundColor, 0 };
                for (int i = 0; i < 9 && colorsCount[i].count != 0; i++)
                    if (colorsCount[i].count > dominantColorCount.count) dominantColorCount = colorsCount[i];
                // assign the dominant color w/ a bit of randomness to avoid "gliders"
                if (dominantColorCount.count > 0 && random8(128)) SEGMENT.setPixelColorXY(x, y, dominantColorCount.color);
            } else if ((col == bgc) && (neighbors == 2) && !random8(128)) {  // Mutation
                SEGMENT.setPixelColorXY(x, y, SEGMENT.color_from_palette(random8(), false, PALETTE_SOLID_WRAP, 255));
            }
            // else do nothing!
        }  // x,y

    // calculate CRC16 of leds
    uint16_t crc = crc16((const unsigned char*)prevLeds, dataSize);
    // check if we had same CRC and reset if needed
    bool repetition = false;
    for (int i = 0; i < crcBufferLen && !repetition; i++) repetition = (crc == crcBuffer[i]);  // (Ewowi)
    // same CRC would mean image did not change or was repeating itself
    if (!repetition) SEGENV.step = strip.now;  // if no repetition avoid reset
    // remember CRCs across frames
    crcBuffer[SEGENV.aux0] = crc;
    ++SEGENV.aux0 %= crcBufferLen;

    return FRAMETIME;
}

// ---- Hiphotic --------------------------------------------------------------------------
uint16_t mode_2DHiphotic(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int      cols = SEGMENT.virtualWidth();
    const int      rows = SEGMENT.virtualHeight();
    const uint32_t a    = strip.now / ((SEGMENT.custom3 >> 1) + 1);

    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            SEGMENT.setPixelColorXY(x, y,
                                    SEGMENT.color_from_palette(sin8_t(cos8_t(x * SEGMENT.speed / 16 + a / 3) + sin8_t(y * SEGMENT.intensity / 16 + a / 4) + a),
                                                               false, PALETTE_SOLID_WRAP, 0));
        }
    }

    return FRAMETIME;
}

// ---- Julia ------------------------------------------------------------------------------
namespace {
typedef struct Julia {
    float xcen;
    float ycen;
    float xymag;
} julia;
}  // namespace

uint16_t mode_2DJulia(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (!SEGENV.allocateData(sizeof(julia))) return mode_static();
    Julia* julias = reinterpret_cast<Julia*>(SEGENV.data);

    float reAl;
    float imAg;

    if (SEGENV.call == 0) {  // Reset the center if we've just re-started this animation.
        julias->xcen  = 0.;
        julias->ycen  = 0.;
        julias->xymag = 1.0;

        SEGMENT.custom1   = 128;  // Make sure the location widgets are centered to start.
        SEGMENT.custom2   = 128;
        SEGMENT.custom3   = 16;
        SEGMENT.intensity = 24;
    }

    julias->xcen  = julias->xcen + (float)(SEGMENT.custom1 - 128) / 100000.f;
    julias->ycen  = julias->ycen + (float)(SEGMENT.custom2 - 128) / 100000.f;
    julias->xymag = julias->xymag + (float)((SEGMENT.custom3 - 16) << 3) / 100000.f;  // reduced resolution slider
    if (julias->xymag < 0.01f) julias->xymag = 0.01f;
    if (julias->xymag > 1.0f) julias->xymag = 1.0f;

    float xmin = julias->xcen - julias->xymag;
    float xmax = julias->xcen + julias->xymag;
    float ymin = julias->ycen - julias->xymag;
    float ymax = julias->ycen + julias->xymag;

    // Whole set should be within -1.2,1.2 to -.8 to 1.
    xmin = constrain(xmin, -1.2f, 1.2f);
    xmax = constrain(xmax, -1.2f, 1.2f);
    ymin = constrain(ymin, -0.8f, 1.0f);
    ymax = constrain(ymax, -0.8f, 1.0f);

    float dx;  // Delta x is mapped to the matrix size.
    float dy;  // Delta y is mapped to the matrix size.

    int   maxIterations = 15;    // How many iterations per pixel before we give up. Make it 8 bits to match our range of colours.
    float maxCalc       = 16.0;  // How big is each calculation allowed to be before we give up.

    maxIterations = SEGMENT.intensity / 2;

    // Resize section on the fly for some animaton.
    reAl = -0.94299f;  // PixelBlaze example
    imAg = 0.3162f;

    reAl += (float)sin16_t(strip.now * 34) / 655340.f;
    imAg += (float)sin16_t(strip.now * 26) / 655340.f;

    dx = (xmax - xmin) / (cols);  // Scale the delta x and y values to our matrix size.
    dy = (ymax - ymin) / (rows);

    // Start y
    float y = ymin;
    for (int j = 0; j < rows; j++) {
        // Start x
        float x = xmin;
        for (int i = 0; i < cols; i++) {
            // Now we test, as we iterate z = z^2 + c does z tend towards infinity?
            float a    = x;
            float b    = y;
            int   iter = 0;

            while (iter < maxIterations) {  // Here we determine whether or not we're out of bounds.
                float aa  = a * a;
                float bb  = b * b;
                float len = aa + bb;
                if (len > maxCalc) {  // |z| = sqrt(a^2+b^2) OR z^2 = a^2+b^2 to save on having to perform a square root.
                    break;            // Bail
                }

                // This operation corresponds to z -> z^2+c where z=a+ib c=(x,y). Remember to use 'foil'.
                // Two statements: one expression `2*a*b + imAg` may be fused into an FMA (clang
                // contracts within an expression), which the app's float math cannot reproduce.
                float ab2 = 2 * a * b;
                b         = ab2 + imAg;
                a         = aa - bb + reAl;
                iter++;
            }  // while

            // We color each pixel based on how long it takes to get to infinity, or black if it never gets there.
            if (iter == maxIterations) {
                SEGMENT.setPixelColorXY(i, j, 0);
            } else {
                SEGMENT.setPixelColorXY(i, j, SEGMENT.color_from_palette(iter * 255 / maxIterations, false, PALETTE_SOLID_WRAP, 0));
            }
            x += dx;
        }
        y += dy;
    }

    return FRAMETIME;
}

// ---- Lissajous --------------------------------------------------------------------------
uint16_t mode_2DLissajous(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    SEGMENT.fadeToBlackBy(SEGMENT.intensity);
    uint_fast16_t phase = (strip.now * (1 + SEGENV.custom3)) / 32;  // allow user to control rotation speed

    for (int i = 0; i < 256; i++) {
        uint_fast8_t xlocn = sin8_t(phase / 2 + (i * SEGMENT.speed) / 32);
        uint_fast8_t ylocn = cos8_t(phase / 2 + i * 2);
        xlocn              = (cols < 2) ? 1 : (map(2 * xlocn, 0, 511, 0, 2 * (cols - 1)) + 1) / 2;  // softhack007: "(2* ..... +1) /2" for proper rounding
        ylocn              = (rows < 2) ? 1 : (map(2 * ylocn, 0, 511, 0, 2 * (rows - 1)) + 1) / 2;  // "rows > 1" is needed to avoid div/0 in map()
        SEGMENT.setPixelColorXY((uint8_t)xlocn, (uint8_t)ylocn, SEGMENT.color_from_palette(strip.now / 100 + i, false, PALETTE_SOLID_WRAP, 0));
    }

    return FRAMETIME;
}

// ---- Plasma Ball ----------------------------------------------------------------------------
uint16_t mode_2DPlasmaball(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    SEGMENT.fadeToBlackBy(SEGMENT.custom1 >> 2);
    uint_fast32_t t = (strip.now * 8) / (256 - SEGMENT.speed);  // optimized to avoid float
    for (int i = 0; i < cols; i++) {
        unsigned thisVal = inoise8(i * 30, t, t);
        unsigned thisMax = map(thisVal, 0, 255, 0, cols - 1);
        for (int j = 0; j < rows; j++) {
            unsigned thisVal_ = inoise8(t, j * 30, t);
            unsigned thisMax_ = map(thisVal_, 0, 255, 0, rows - 1);
            int      x        = (i + thisMax_ - cols / 2);
            int      y        = (j + thisMax - cols / 2);
            int      cx       = (i + thisMax_);
            int      cy       = (j + thisMax);

            SEGMENT.addPixelColorXY(i, j,
                                    ((x - y > -2) && (x - y < 2)) || ((cols - 1 - x - y) > -2 && (cols - 1 - x - y < 2)) || (cols - cx == 0) ||
                                            (cols - 1 - cx == 0) || ((rows - cy == 0) || (rows - 1 - cy == 0))
                                        ? ColorFromPalette(SEGPALETTE, beat8(5), thisVal, LINEARBLEND)
                                        : CRGB::Black);
        }
    }
    SEGMENT.blur(SEGMENT.custom2 >> 5);

    return FRAMETIME;
}

// ---- Sindots ----------------------------------------------------------------------------------
uint16_t mode_2DSindots(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
    }

    SEGMENT.fadeToBlackBy(SEGMENT.custom1 >> 3);

    byte t1 = strip.now / (257 - SEGMENT.speed);  // 20;
    byte t2 = sin8_t(t1) / 4 * 2;
    for (int i = 0; i < 13; i++) {
        int x = sin8_t(t1 + i * SEGMENT.intensity / 8) * (cols - 1) / 255;  // max index now 255x15/255=15!
        int y = sin8_t(t2 + i * SEGMENT.intensity / 8) * (rows - 1) / 255;  // max index now 255x15/255=15!
        SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, i * 255 / 13, 255, LINEARBLEND));
    }
    SEGMENT.blur(SEGMENT.custom2 >> 3);

    return FRAMETIME;
}

// ---- Sun Radiation -------------------------------------------------------------------------------
uint16_t mode_2DSunradiation(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (!SEGENV.allocateData(sizeof(byte) * (cols + 2) * (rows + 2))) return mode_static();  // allocation failed
    byte* bump = reinterpret_cast<byte*>(SEGENV.data);

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
    }

    unsigned long t       = strip.now / 4;
    unsigned      index   = 0;
    uint8_t       someVal = SEGMENT.speed / 4;  // Was 25.
    for (int j = 0; j < (rows + 2); j++) {
        for (int i = 0; i < (cols + 2); i++) {
            byte col      = (inoise8_raw(i * someVal, j * someVal, t)) / 2;
            bump[index++] = col;
        }
    }

    int yindex = cols + 3;
    int vly    = -(rows / 2 + 1);
    for (int y = 0; y < rows; y++) {
        ++vly;
        int vlx = -(cols / 2 + 1);
        for (int x = 0; x < cols; x++) {
            ++vlx;
            int      nx   = bump[x + yindex + 1] - bump[x + yindex - 1];
            int      ny   = bump[x + yindex + (cols + 2)] - bump[x + yindex - (cols + 2)];
            unsigned difx = abs8(vlx * 7 - nx);
            unsigned dify = abs8(vly * 7 - ny);
            int      temp = difx * difx + dify * dify;
            int      col  = 255 - temp / 8;  // 8 its a size of effect
            if (col < 0) col = 0;
            SEGMENT.setPixelColorXY(x, y, HeatColor(col / (3.0f - (float)(SEGMENT.intensity) / 128.f)));
        }
        yindex += (cols + 2);
    }

    return FRAMETIME;
}

// ---- Tartan -------------------------------------------------------------------------------------------
uint16_t mode_2Dtartan(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
    }

    uint8_t hue, bri;
    size_t  intensity;
    int     offsetX   = beatsin16_t(3, -360, 360);
    int     offsetY   = beatsin16_t(2, -360, 360);
    int     sharpness = SEGMENT.custom3 / 8;  // 0-3

    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            hue       = x * beatsin16_t(10, 1, 10) + offsetY;
            intensity = bri = sin8_t(x * SEGMENT.speed / 2 + offsetX);
            for (int i = 0; i < sharpness; i++) intensity *= bri;
            intensity >>= 8 * sharpness;
            SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, hue, intensity, LINEARBLEND));
            hue       = y * 3 + offsetX;
            intensity = bri = sin8_t(y * SEGMENT.intensity / 2 + offsetY);
            for (int i = 0; i < sharpness; i++) intensity *= bri;
            intensity >>= 8 * sharpness;
            SEGMENT.addPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, hue, intensity, LINEARBLEND));
        }
    }

    return FRAMETIME;
}

// ---- Soap ----------------------------------------------------------------------------------------------
uint16_t mode_2Dsoap(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    const size_t dataSize = SEGMENT.width() * SEGMENT.height() * sizeof(uint8_t);  // prevent reallocation if mirrored or grouped
    if (!SEGENV.allocateData(dataSize + sizeof(uint32_t) * 3)) return mode_static();  // allocation failed

    uint8_t*       noise3d    = reinterpret_cast<uint8_t*>(SEGENV.data);
    uint32_t*      noise32_x  = reinterpret_cast<uint32_t*>(SEGENV.data + dataSize);
    uint32_t*      noise32_y  = reinterpret_cast<uint32_t*>(SEGENV.data + dataSize + sizeof(uint32_t));
    uint32_t*      noise32_z  = reinterpret_cast<uint32_t*>(SEGENV.data + dataSize + sizeof(uint32_t) * 2);
    const uint32_t scale32_x  = 160000U / cols;
    const uint32_t scale32_y  = 160000U / rows;
    const uint32_t mov        = MIN(cols, rows) * (SEGMENT.speed + 2) / 2;
    const uint8_t  smoothness = MIN(250, SEGMENT.intensity);  // limit as >250 produces very little changes

    // init
    if (SEGENV.call == 0) {
        *noise32_x = random16();
        *noise32_y = random16();
        *noise32_z = random16();
    } else {
        *noise32_x += mov;
        *noise32_y += mov;
        *noise32_z += mov;
    }

    for (int i = 0; i < cols; i++) {
        int32_t ioffset = scale32_x * (i - cols / 2);
        for (int j = 0; j < rows; j++) {
            int32_t joffset  = scale32_y * (j - rows / 2);
            uint8_t data     = inoise16(*noise32_x + ioffset, *noise32_y + joffset, *noise32_z) >> 8;
            noise3d[XY(i, j)] = scale8(noise3d[XY(i, j)], smoothness) + scale8(data, 255 - smoothness);
        }
    }
    // init also if dimensions changed
    if (SEGENV.call == 0 || SEGMENT.aux0 != cols || SEGMENT.aux1 != rows) {
        SEGMENT.aux0 = cols;
        SEGMENT.aux1 = rows;
        for (int i = 0; i < cols; i++) {
            for (int j = 0; j < rows; j++) {
                SEGMENT.setPixelColorXY(i, j, ColorFromPalette(SEGPALETTE, ~noise3d[XY(i, j)] * 3));
            }
        }
    }

    int zD;
    int zF;
    int amplitude;
    int shiftX = 0;  //(SEGMENT.custom1 - 128) / 4;
    int shiftY = 0;  //(SEGMENT.custom2 - 128) / 4;
    // WLED's `CRGB ledsbuff[MAX(cols,rows)]` is a VLA of a class type, which clang
    // rejects; alloca is the same stack buffer.  Every entry is written before it is read.
    CRGB* ledsbuff = static_cast<CRGB*>(alloca(sizeof(CRGB) * MAX(cols, rows)));

    amplitude = (cols >= 16) ? (cols - 8) / 8 : 1;
    for (int y = 0; y < rows; y++) {
        int amount   = ((int)noise3d[XY(0, y)] - 128) * 2 * amplitude + 256 * shiftX;
        int delta    = abs(amount) >> 8;
        int fraction = abs(amount) & 255;
        for (int x = 0; x < cols; x++) {
            if (amount < 0) {
                zD = x - delta;
                zF = zD - 1;
            } else {
                zD = x + delta;
                zF = zD + 1;
            }
            CRGB PixelA = CRGB::Black;
            if ((zD >= 0) && (zD < cols))
                PixelA = SEGMENT.getPixelColorXY(zD, y);
            else
                PixelA = ColorFromPalette(SEGPALETTE, ~noise3d[XY(abs(zD), y)] * 3);
            CRGB PixelB = CRGB::Black;
            if ((zF >= 0) && (zF < cols))
                PixelB = SEGMENT.getPixelColorXY(zF, y);
            else
                PixelB = ColorFromPalette(SEGPALETTE, ~noise3d[XY(abs(zF), y)] * 3);
            ledsbuff[x] = (PixelA.nscale8(ease8InOutApprox(255 - fraction))) + (PixelB.nscale8(ease8InOutApprox(fraction)));
        }
        for (int x = 0; x < cols; x++) SEGMENT.setPixelColorXY(x, y, ledsbuff[x]);
    }

    amplitude = (rows >= 16) ? (rows - 8) / 8 : 1;
    for (int x = 0; x < cols; x++) {
        int amount   = ((int)noise3d[XY(x, 0)] - 128) * 2 * amplitude + 256 * shiftY;
        int delta    = abs(amount) >> 8;
        int fraction = abs(amount) & 255;
        for (int y = 0; y < rows; y++) {
            if (amount < 0) {
                zD = y - delta;
                zF = zD - 1;
            } else {
                zD = y + delta;
                zF = zD + 1;
            }
            CRGB PixelA = CRGB::Black;
            if ((zD >= 0) && (zD < rows))
                PixelA = SEGMENT.getPixelColorXY(x, zD);
            else
                PixelA = ColorFromPalette(SEGPALETTE, ~noise3d[XY(x, abs(zD))] * 3);
            CRGB PixelB = CRGB::Black;
            if ((zF >= 0) && (zF < rows))
                PixelB = SEGMENT.getPixelColorXY(x, zF);
            else
                PixelB = ColorFromPalette(SEGPALETTE, ~noise3d[XY(x, abs(zF))] * 3);
            ledsbuff[y] = (PixelA.nscale8(ease8InOutApprox(255 - fraction))) + (PixelB.nscale8(ease8InOutApprox(fraction)));
        }
        for (int y = 0; y < rows; y++) SEGMENT.setPixelColorXY(x, y, ledsbuff[y]);
    }

    return FRAMETIME;
}

// ---- Octopus ---------------------------------------------------------------------------------------------
uint16_t mode_2Doctopus(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int     cols = SEGMENT.virtualWidth();
    const int     rows = SEGMENT.virtualHeight();
    const uint8_t mapp = 180 / MAX(cols, rows);

    typedef struct {
        uint8_t angle;
        uint8_t radius;
    } map_t;

    const size_t dataSize = SEGMENT.width() * SEGMENT.height() * sizeof(map_t);  // prevent reallocation if mirrored or grouped
    if (!SEGENV.allocateData(dataSize + 2)) return mode_static();  // allocation failed

    map_t*   rMap  = reinterpret_cast<map_t*>(SEGENV.data);
    uint8_t* offsX = reinterpret_cast<uint8_t*>(SEGENV.data + dataSize);
    uint8_t* offsY = reinterpret_cast<uint8_t*>(SEGENV.data + dataSize + 1);

    // re-init if SEGMENT dimensions or offset changed
    if (SEGENV.call == 0 || SEGENV.aux0 != cols || SEGENV.aux1 != rows || SEGMENT.custom1 != *offsX || SEGMENT.custom2 != *offsY) {
        SEGENV.step = 0;  // t
        SEGENV.aux0 = cols;
        SEGENV.aux1 = rows;
        *offsX      = SEGMENT.custom1;
        *offsY      = SEGMENT.custom2;
        const int C_X = (cols / 2) + ((SEGMENT.custom1 - 128) * cols) / 255;
        const int C_Y = (rows / 2) + ((SEGMENT.custom2 - 128) * rows) / 255;
        for (int x = 0; x < cols; x++) {
            for (int y = 0; y < rows; y++) {
                int dx                = (x - C_X);
                int dy                = (y - C_Y);
                rMap[XY(x, y)].angle  = int(40.7436f * atan2_t(dy, dx));  // avoid 128*atan2()/PI
                rMap[XY(x, y)].radius = sqrtf(dx * dx + dy * dy) * mapp;  // thanks Sutaburosu
            }
        }
    }

    SEGENV.step += SEGMENT.speed / 32 + 1;  // 1-4 range
    for (int x = 0; x < cols; x++) {
        for (int y = 0; y < rows; y++) {
            byte     angle     = rMap[XY(x, y)].angle;
            byte     radius    = rMap[XY(x, y)].radius;
            unsigned intensity = sin8_t(sin8_t((angle * 4 - radius) / 4 + SEGENV.step / 2) + radius - SEGENV.step + angle * (SEGMENT.custom3 / 4 + 1));
            intensity          = map((intensity * intensity) & 0xFFFF, 0, 65535, 0, 255);  // add a bit of non-linearity for cleaner display
            CRGB c             = ColorFromPalette(SEGPALETTE, SEGENV.step / 2 - radius, intensity);
            SEGMENT.setPixelColorXY(x, y, c);
        }
    }
    return FRAMETIME;
}

// ---- Waving Cell -------------------------------------------------------------------------------------------
uint16_t mode_2Dwavingcell(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    uint32_t t  = strip.now / (257 - SEGMENT.speed);
    uint8_t  aX = SEGMENT.custom1 / 16 + 9;
    uint8_t  aY = SEGMENT.custom2 / 16 + 1;
    uint8_t  aZ = SEGMENT.custom3 + 1;
    for (int x = 0; x < cols; x++)
        for (int y = 0; y < rows; y++)
            SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, ((sin8_t((x * aX) + sin8_t((y + t) * aY)) + cos8_t(y * aZ)) + 1) + t));

    return FRAMETIME;
}

#undef XY

}  // namespace wled
