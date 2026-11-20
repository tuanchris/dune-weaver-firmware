// WLED effects, ported from WLED v0.15.3 wled00/FX.cpp (Copyright (c) Harm
// Aldick, Christian Schwinne and WLED contributors; EUPL-1.2, redistributed
// under GPLv3 per its Article 5).  Mirrored by
// dune-weaver-mobile/src/lib/wledfx/fx_g.ts; tools/fx_parity.py checks them.
//
// Batch g: 2D matrix effects (drawn on the runner's 16x16 matrix).
#include "WledFx.h"


namespace wled {

// Arduino's radians(): deg * DEG_TO_RAD, in double
static inline double radians(double deg) {
    return deg * 0.017453292519943295769236907684886;
}

#define XY(x, y) SEGMENT.XY(x, y)

// ---- DNA ----------------------------------------------------------------------
uint16_t mode_2Ddna(void) {  // dna originally by by ldirko at https://pastebin.com/pCkkkzcs. Updated by Preyy. WLED conversion by Andrew Tuline.
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    SEGMENT.fadeToBlackBy(64);
    for (int i = 0; i < cols; i++) {
        SEGMENT.setPixelColorXY(i, beatsin8_t(SEGMENT.speed / 8, 0, rows - 1, 0, i * 4),
                                ColorFromPalette(SEGPALETTE, i * 5 + strip.now / 17, beatsin8_t(5, 55, 255, 0, i * 10), LINEARBLEND));
        SEGMENT.setPixelColorXY(i, beatsin8_t(SEGMENT.speed / 8, 0, rows - 1, 0, i * 4 + 128),
                                ColorFromPalette(SEGPALETTE, i * 5 + 128 + strip.now / 17, beatsin8_t(5, 55, 255, 0, i * 10 + 128), LINEARBLEND));
    }
    SEGMENT.blur(SEGMENT.intensity >> 3);

    return FRAMETIME;
}

// ---- Drift --------------------------------------------------------------------
uint16_t mode_2DDrift(void) {  // By: Stepko   https://editor.soulmatelights.com/gallery/884-drift , Modified by: Andrew Tuline
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    const int colsCenter = (cols >> 1) + (cols % 2);
    const int rowsCenter = (rows >> 1) + (rows % 2);

    SEGMENT.fadeToBlackBy(128);
    const float   maxDim = MAX(cols, rows) / 2;
    unsigned long t      = strip.now / (32 - (SEGMENT.speed >> 3));
    unsigned long t_20   = t / 20;  // softhack007: pre-calculating this gives about 10% speedup
    for (float i = 1.0f; i < maxDim; i += 0.25f) {
        float angle = radians(t * (maxDim - i));
        int   mySin = sin_t(angle) * i;
        int   myCos = cos_t(angle) * i;
        // (uint32_t): the float index grows with time; uint8_t of it would be UB past 255
        SEGMENT.setPixelColorXY(colsCenter + mySin, rowsCenter + myCos, ColorFromPalette(SEGPALETTE, (uint32_t)((i * 20) + t_20), 255, LINEARBLEND));
        if (SEGMENT.check1)
            SEGMENT.setPixelColorXY(colsCenter + myCos, rowsCenter + mySin, ColorFromPalette(SEGPALETTE, (uint32_t)((i * 20) + t_20), 255, LINEARBLEND));
    }
    SEGMENT.blur(SEGMENT.intensity >> 3);

    return FRAMETIME;
}

// ---- Firenoise ----------------------------------------------------------------
uint16_t mode_2Dfirenoise(void) {  // firenoise2d. By Andrew Tuline. Yet another short routine.
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
    }

    unsigned xscale = SEGMENT.intensity * 4;
    unsigned yscale = SEGMENT.speed * 8;
    unsigned indexx = 0;

    // FastLED's 16-color constructor, as the layer's brace list
    CRGBPalette16 pal = SEGMENT.check1 ? SEGPALETTE
                                       : CRGBPalette16{ CRGB::Black,      CRGB::Black,      CRGB::Black,  CRGB::Black,
                                                        CRGB::Red,        CRGB::Red,        CRGB::Red,    CRGB::DarkOrange,
                                                        CRGB::DarkOrange, CRGB::DarkOrange, CRGB::Orange, CRGB::Orange,
                                                        CRGB::Yellow,     CRGB::Orange,     CRGB::Yellow, CRGB::Yellow };

    for (int j = 0; j < cols; j++) {
        for (int i = 0; i < rows; i++) {
            indexx = inoise8(j * yscale * rows / 255, i * xscale + strip.now / 4);  // We're moving along our Perlin map.
            SEGMENT.setPixelColorXY(j, i, ColorFromPalette(pal, min(i * (indexx) >> 4, 255U), i * 255 / cols, LINEARBLEND));
        }
    }

    return FRAMETIME;
}

// ---- Matrix -------------------------------------------------------------------
uint16_t mode_2Dmatrix(void) {  // Matrix2D. By Jeremy Williams. Adapted by Andrew Tuline & improved by merkisoft and ewowi, and softhack007.
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    unsigned dataSize = (SEGMENT.length() + 7) >> 3;            // 1 bit per LED for trails
    if (!SEGENV.allocateData(dataSize)) return mode_static();  // allocation failed

    if (SEGENV.call == 0) {
        SEGMENT.fill(BLACK);
        SEGENV.step = 0;
    }

    uint8_t fade  = map(SEGMENT.custom1, 0, 255, 50, 250);                       // equals trail size
    uint8_t speed = (256 - SEGMENT.speed) >> map(min(rows, 150), 0, 150, 0, 3);  // slower speeds for small displays

    uint32_t spawnColor;
    uint32_t trailColor;
    if (SEGMENT.check1) {
        spawnColor = SEGCOLOR(0);
        trailColor = SEGCOLOR(1);
    } else {
        spawnColor = RGBW32(175, 255, 175, 0);
        trailColor = RGBW32(27, 130, 39, 0);
    }

    bool emptyScreen = true;
    if (strip.now - SEGENV.step >= speed) {
        SEGENV.step = strip.now;
        // move pixels one row down. Falling codes keep color and add trail pixels; all others pixels are faded
        SEGMENT.fadeToBlackBy(fade);
        for (int row = rows - 1; row >= 0; row--) {
            for (int col = 0; col < cols; col++) {
                unsigned index  = XY(col, row) >> 3;
                unsigned bitNum = XY(col, row) & 0x07;
                if (bitRead(SEGENV.data[index], bitNum)) {
                    SEGMENT.setPixelColorXY(col, row, trailColor);  // create trail
                    bitClear(SEGENV.data[index], bitNum);
                    if (row < rows - 1) {
                        SEGMENT.setPixelColorXY(col, row + 1, spawnColor);
                        index  = XY(col, row + 1) >> 3;
                        bitNum = XY(col, row + 1) & 0x07;
                        bitSet(SEGENV.data[index], bitNum);
                        emptyScreen = false;
                    }
                }
            }
        }

        // spawn new falling code
        if (random8() <= SEGMENT.intensity || emptyScreen) {
            uint8_t spawnX = random8(cols);
            SEGMENT.setPixelColorXY(spawnX, 0, spawnColor);
            // update hint for next run
            unsigned index  = XY(spawnX, 0) >> 3;
            unsigned bitNum = XY(spawnX, 0) & 0x07;
            bitSet(SEGENV.data[index], bitNum);
        }
    }

    return FRAMETIME;
}

// ---- Metaballs ----------------------------------------------------------------
uint16_t mode_2Dmetaballs(void) {  // Metaballs by Stefan Petrick. Cannot have one of the dimensions be 2 or less. Adapted by Andrew Tuline.
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    float speed = 0.25f * (1 + (SEGMENT.speed >> 6));

    // get some 2 random moving points
    // (uint32_t): inoise8 takes uint16_t, and a float past 65535 would be UB (the ESP32 wraps it)
    int x2 = map(inoise8((uint32_t)(strip.now * speed), 25355, 685), 0, 255, 0, cols - 1);
    int y2 = map(inoise8((uint32_t)(strip.now * speed), 355, 11685), 0, 255, 0, rows - 1);

    int x3 = map(inoise8((uint32_t)(strip.now * speed), 55355, 6685), 0, 255, 0, cols - 1);
    int y3 = map(inoise8((uint32_t)(strip.now * speed), 25355, 22685), 0, 255, 0, rows - 1);

    // and one Lissajou function
    int x1 = beatsin8_t(23 * speed, 0, cols - 1);
    int y1 = beatsin8_t(28 * speed, 0, rows - 1);

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            // calculate distances of the 3 points from actual pixel
            // and add them together with weightening
            unsigned dx   = abs(x - x1);
            unsigned dy   = abs(y - y1);
            unsigned dist = 2 * sqrt16((dx * dx) + (dy * dy));

            dx = abs(x - x2);
            dy = abs(y - y2);
            dist += sqrt16((dx * dx) + (dy * dy));

            dx = abs(x - x3);
            dy = abs(y - y3);
            dist += sqrt16((dx * dx) + (dy * dy));

            // inverse result
            int color = dist ? 1000 / dist : 255;

            // map color between thresholds
            if (color > 0 and color < 60) {
                SEGMENT.setPixelColorXY(x, y, SEGMENT.color_from_palette(map(color * 9, 9, 531, 0, 255), false, PALETTE_SOLID_WRAP, 0));
            } else {
                SEGMENT.setPixelColorXY(x, y, SEGMENT.color_from_palette(0, false, PALETTE_SOLID_WRAP, 0));
            }
            // show the 3 points, too
            SEGMENT.setPixelColorXY(x1, y1, WHITE);
            SEGMENT.setPixelColorXY(x2, y2, WHITE);
            SEGMENT.setPixelColorXY(x3, y3, WHITE);
        }
    }

    return FRAMETIME;
}

// ---- Noise2D ------------------------------------------------------------------
uint16_t mode_2Dnoise(void) {  // By Andrew Tuline
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    const unsigned scale = SEGMENT.intensity + 2;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            uint8_t pixelHue8 = inoise8(x * scale, y * scale, strip.now / (16 - SEGMENT.speed / 16));
            SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, pixelHue8));
        }
    }

    return FRAMETIME;
}

// ---- Pulser -------------------------------------------------------------------
uint16_t mode_2DPulser(void) {  // By: ldirko   https://editor.soulmatelights.com/gallery/878-pulse-test , modifed by: Andrew Tuline
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    SEGMENT.fadeToBlackBy(8 - (SEGMENT.intensity >> 5));
    uint32_t a = strip.now / (18 - SEGMENT.speed / 16);
    int      x = (a / 14) % cols;
    int      y = map((sin8_t(a * 5) + sin8_t(a * 4) + sin8_t(a * 2)), 0, 765, rows - 1, 0);
    SEGMENT.setPixelColorXY(x, y, ColorFromPalette(SEGPALETTE, map(y, 0, rows - 1, 0, 255), 255, LINEARBLEND));

    SEGMENT.blur(SEGMENT.intensity >> 4);

    return FRAMETIME;
}

// ---- Squared Swirl ------------------------------------------------------------
// custom3 affects the blur amount.
uint16_t mode_2Dsquaredswirl(void) {  // By: Mark Kriegsman. https://gist.github.com/kriegsman/368b316c55221134b160
                                      // Modifed by: Andrew Tuline
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    const uint8_t kBorderWidth = 2;

    SEGMENT.fadeToBlackBy(24);
    SEGMENT.blur(SEGMENT.custom3 >> 1);

    // Use two out-of-sync sine waves
    int i = beatsin8_t(19, kBorderWidth, cols - kBorderWidth);
    int j = beatsin8_t(22, kBorderWidth, cols - kBorderWidth);
    int k = beatsin8_t(17, kBorderWidth, cols - kBorderWidth);
    int m = beatsin8_t(18, kBorderWidth, rows - kBorderWidth);
    int n = beatsin8_t(15, kBorderWidth, rows - kBorderWidth);
    int p = beatsin8_t(20, kBorderWidth, rows - kBorderWidth);

    SEGMENT.addPixelColorXY(i, m, ColorFromPalette(SEGPALETTE, strip.now / 29, 255, LINEARBLEND));
    SEGMENT.addPixelColorXY(j, n, ColorFromPalette(SEGPALETTE, strip.now / 41, 255, LINEARBLEND));
    SEGMENT.addPixelColorXY(k, p, ColorFromPalette(SEGPALETTE, strip.now / 73, 255, LINEARBLEND));

    return FRAMETIME;
}

// ---- Spaceships ---------------------------------------------------------------
uint16_t mode_2Dspaceships(void) {  //// Space ships by stepko (c)05.02.21 [https://editor.soulmatelights.com/gallery/639-space-ships], adapted by Blaz Kristan (AKA blazoncek)
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    uint32_t tb = strip.now >> 12;  // every ~4s
    if (tb > SEGENV.step) {
        int dir = ++SEGENV.aux0;
        dir += (int)random8(3) - 1;
        if (dir > 7)
            SEGENV.aux0 = 0;
        else if (dir < 0)
            SEGENV.aux0 = 7;
        else
            SEGENV.aux0 = dir;
        SEGENV.step = tb + random8(4);
    }

    SEGMENT.fadeToBlackBy(map(SEGMENT.speed, 0, 255, 248, 16));
    SEGMENT.move(SEGENV.aux0, 1);

    for (size_t i = 0; i < 8; i++) {
        int  x     = beatsin8_t(12 + i, 2, cols - 3);
        int  y     = beatsin8_t(15 + i, 2, rows - 3);
        CRGB color = ColorFromPalette(SEGPALETTE, beatsin8_t(12 + i, 0, 255), 255);
        SEGMENT.addPixelColorXY(x, y, color);
        if (cols > 24 || rows > 24) {
            SEGMENT.addPixelColorXY(x + 1, y, color);
            SEGMENT.addPixelColorXY(x - 1, y, color);
            SEGMENT.addPixelColorXY(x, y + 1, color);
            SEGMENT.addPixelColorXY(x, y - 1, color);
        }
    }
    SEGMENT.blur(SEGMENT.intensity >> 3);

    return FRAMETIME;
}

// ---- Crazy Bees ---------------------------------------------------------------
//// Crazy bees by stepko (c)12.02.21 [https://editor.soulmatelights.com/gallery/651-crazy-bees], adapted by Blaz Kristan (AKA blazoncek)
#define MAX_BEES 5
uint16_t mode_2Dcrazybees(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    byte n = MIN(MAX_BEES, (rows * cols) / 256 + 1);

    typedef struct Bee {
        uint8_t posX, posY, aimX, aimY, hue;
        int8_t  deltaX, deltaY, signX, signY, error;
        void    aimed(uint16_t w, uint16_t h) {
            //random16_set_seed(millis());
            aimX   = random8(0, w);
            aimY   = random8(0, h);
            hue    = random8();
            deltaX = abs(aimX - posX);
            deltaY = abs(aimY - posY);
            signX  = posX < aimX ? 1 : -1;
            signY  = posY < aimY ? 1 : -1;
            error  = deltaX - deltaY;
        };
    } bee_t;

    if (!SEGENV.allocateData(sizeof(bee_t) * MAX_BEES)) return mode_static();  // allocation failed
    bee_t* bee = reinterpret_cast<bee_t*>(SEGENV.data);

    if (SEGENV.call == 0) {
        random16_set_seed(strip.now);
        for (size_t i = 0; i < n; i++) {
            bee[i].posX = random8(0, cols);
            bee[i].posY = random8(0, rows);
            bee[i].aimed(cols, rows);
        }
    }

    if (strip.now > SEGENV.step) {
        SEGENV.step = strip.now + (FRAMETIME * 16 / ((SEGMENT.speed >> 4) + 1));

        SEGMENT.fadeToBlackBy(32);

        for (size_t i = 0; i < n; i++) {
            SEGMENT.addPixelColorXY(bee[i].aimX + 1, bee[i].aimY, CHSV(bee[i].hue, 255, 255));
            SEGMENT.addPixelColorXY(bee[i].aimX, bee[i].aimY + 1, CHSV(bee[i].hue, 255, 255));
            SEGMENT.addPixelColorXY(bee[i].aimX - 1, bee[i].aimY, CHSV(bee[i].hue, 255, 255));
            SEGMENT.addPixelColorXY(bee[i].aimX, bee[i].aimY - 1, CHSV(bee[i].hue, 255, 255));
            if (bee[i].posX != bee[i].aimX || bee[i].posY != bee[i].aimY) {
                SEGMENT.setPixelColorXY(bee[i].posX, bee[i].posY, CRGB(CHSV(bee[i].hue, 60, 255)));
                int error2 = bee[i].error * 2;
                if (error2 > -bee[i].deltaY) {
                    bee[i].error -= bee[i].deltaY;
                    bee[i].posX += bee[i].signX;
                }
                if (error2 < bee[i].deltaX) {
                    bee[i].error += bee[i].deltaX;
                    bee[i].posY += bee[i].signY;
                }
            } else {
                bee[i].aimed(cols, rows);
            }
        }
        SEGMENT.blur(SEGMENT.intensity >> 4);
    }
    return FRAMETIME;
}
#undef MAX_BEES

// ---- Ghost Rider --------------------------------------------------------------
//// Ghost Rider by stepko (c)2021 [https://editor.soulmatelights.com/gallery/716-ghost-rider], adapted by Blaz Kristan (AKA blazoncek)
#define LIGHTERS_AM 64  // max lighters (adequate for 32x32 matrix)
uint16_t mode_2Dghostrider(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    typedef struct Lighter {
        int16_t  gPosX;
        int16_t  gPosY;
        uint16_t gAngle;
        int8_t   angleSpeed;
        uint16_t lightersPosX[LIGHTERS_AM];
        uint16_t lightersPosY[LIGHTERS_AM];
        uint16_t Angle[LIGHTERS_AM];
        uint16_t time[LIGHTERS_AM];
        bool     reg[LIGHTERS_AM];
        int8_t   Vspeed;
    } lighter_t;

    if (!SEGENV.allocateData(sizeof(lighter_t))) return mode_static();  // allocation failed
    lighter_t* lighter = reinterpret_cast<lighter_t*>(SEGENV.data);

    const size_t maxLighters = min(cols + rows, LIGHTERS_AM);

    if (SEGENV.aux0 != cols || SEGENV.aux1 != rows) {
        SEGENV.aux0 = cols;
        SEGENV.aux1 = rows;
        //random16_set_seed(strip.now);
        lighter->angleSpeed = random8(0, 20) - 10;
        lighter->gAngle     = random16();
        lighter->Vspeed     = 5;
        lighter->gPosX      = (cols / 2) * 10;
        lighter->gPosY      = (rows / 2) * 10;
        for (size_t i = 0; i < maxLighters; i++) {
            lighter->lightersPosX[i] = lighter->gPosX;
            lighter->lightersPosY[i] = lighter->gPosY + i;
            lighter->time[i]         = i * 2;
            lighter->reg[i]          = false;
        }
    }

    if (strip.now > SEGENV.step) {
        SEGENV.step = strip.now + 1024 / (cols + rows);

        SEGMENT.fadeToBlackBy((SEGMENT.speed >> 2) + 64);

        CRGB color = CRGB::White;
        SEGMENT.wu_pixel(lighter->gPosX * 256 / 10, lighter->gPosY * 256 / 10, color);

        lighter->gPosX += lighter->Vspeed * sin_t(radians(lighter->gAngle));
        lighter->gPosY += lighter->Vspeed * cos_t(radians(lighter->gAngle));
        lighter->gAngle += lighter->angleSpeed;
        if (lighter->gPosX < 0) lighter->gPosX = (cols - 1) * 10;
        if (lighter->gPosX > (cols - 1) * 10) lighter->gPosX = 0;
        if (lighter->gPosY < 0) lighter->gPosY = (rows - 1) * 10;
        if (lighter->gPosY > (rows - 1) * 10) lighter->gPosY = 0;
        for (size_t i = 0; i < maxLighters; i++) {
            lighter->time[i] += random8(5, 20);
            if (lighter->time[i] >= 255 || (lighter->lightersPosX[i] <= 0) || (lighter->lightersPosX[i] >= (cols - 1) * 10) ||
                (lighter->lightersPosY[i] <= 0) || (lighter->lightersPosY[i] >= (rows - 1) * 10)) {
                lighter->reg[i] = true;
            }
            if (lighter->reg[i]) {
                lighter->lightersPosY[i] = lighter->gPosY;
                lighter->lightersPosX[i] = lighter->gPosX;
                lighter->Angle[i]        = lighter->gAngle + ((int)random8(20) - 10);
                lighter->time[i]         = 0;
                lighter->reg[i]          = false;
            } else {
                // `uint16_t += negative float` is UB; (int) first is what the ESP32 does (utrunc.s wraps negatives)
                lighter->lightersPosX[i] = (int)(lighter->lightersPosX[i] + -7 * sin_t(radians(lighter->Angle[i])));
                lighter->lightersPosY[i] = (int)(lighter->lightersPosY[i] + -7 * cos_t(radians(lighter->Angle[i])));
            }
            SEGMENT.wu_pixel(lighter->lightersPosX[i] * 256 / 10, lighter->lightersPosY[i] * 256 / 10, ColorFromPalette(SEGPALETTE, (256 - lighter->time[i])));
        }
        SEGMENT.blur(SEGMENT.intensity >> 3);
    }

    return FRAMETIME;
}
#undef LIGHTERS_AM

// ---- Blobs --------------------------------------------------------------------
//// Floating Blobs by stepko (c)2021 [https://editor.soulmatelights.com/gallery/573-blobs], adapted by Blaz Kristan (AKA blazoncek)
#define MAX_BLOBS 8
uint16_t mode_2Dfloatingblobs(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    typedef struct Blob {
        float x[MAX_BLOBS], y[MAX_BLOBS];
        float sX[MAX_BLOBS], sY[MAX_BLOBS];  // speed
        float r[MAX_BLOBS];
        bool  grow[MAX_BLOBS];
        byte  color[MAX_BLOBS];
    } blob_t;

    size_t Amount = (SEGMENT.intensity >> 5) + 1;  // NOTE: be sure to update MAX_BLOBS if you change this

    if (!SEGENV.allocateData(sizeof(blob_t))) return mode_static();  // allocation failed
    blob_t* blob = reinterpret_cast<blob_t*>(SEGENV.data);

    if (SEGENV.aux0 != cols || SEGENV.aux1 != rows) {
        SEGENV.aux0 = cols;  // re-initialise if virtual size changes
        SEGENV.aux1 = rows;
        //SEGMENT.fill(BLACK);
        for (size_t i = 0; i < MAX_BLOBS; i++) {
            blob->r[i]     = random8(1, cols > 8 ? (cols / 4) : 2);
            blob->sX[i]    = (float)random8(3, cols) / (float)(256 - SEGMENT.speed);  // speed x
            blob->sY[i]    = (float)random8(3, rows) / (float)(256 - SEGMENT.speed);  // speed y
            blob->x[i]     = random8(0, cols - 1);
            blob->y[i]     = random8(0, rows - 1);
            blob->color[i] = random8();
            blob->grow[i]  = (blob->r[i] < 1.f);
            if (blob->sX[i] == 0) blob->sX[i] = 1;
            if (blob->sY[i] == 0) blob->sY[i] = 1;
        }
    }

    SEGMENT.fadeToBlackBy((SEGMENT.custom2 >> 3) + 1);

    // Bounce balls around
    for (size_t i = 0; i < Amount; i++) {
        if (SEGENV.step < strip.now) blob->color[i] = add8(blob->color[i], 4);  // slowly change color
        // change radius if needed
        if (blob->grow[i]) {
            // enlarge radius until it is >= 4
            blob->r[i] += (fabsf(blob->sX[i]) > fabsf(blob->sY[i]) ? fabsf(blob->sX[i]) : fabsf(blob->sY[i])) * 0.05f;
            if (blob->r[i] >= MIN(cols / 4.f, 2.f)) {
                blob->grow[i] = false;
            }
        } else {
            // reduce radius until it is < 1
            blob->r[i] -= (fabsf(blob->sX[i]) > fabsf(blob->sY[i]) ? fabsf(blob->sX[i]) : fabsf(blob->sY[i])) * 0.05f;
            if (blob->r[i] < 1.f) {
                blob->grow[i] = true;
            }
        }
        uint32_t c = SEGMENT.color_from_palette(blob->color[i], false, false, 0);
        if (blob->r[i] > 1.f)
            SEGMENT.fillCircle(roundf(blob->x[i]), roundf(blob->y[i]), roundf(blob->r[i]), c);
        else
            SEGMENT.setPixelColorXY((int)roundf(blob->x[i]), (int)roundf(blob->y[i]), c);
        // move x
        if (blob->x[i] + blob->r[i] >= cols - 1)
            blob->x[i] += (blob->sX[i] * ((cols - 1 - blob->x[i]) / blob->r[i] + 0.005f));
        else if (blob->x[i] - blob->r[i] <= 0)
            blob->x[i] += (blob->sX[i] * (blob->x[i] / blob->r[i] + 0.005f));
        else
            blob->x[i] += blob->sX[i];
        // move y
        if (blob->y[i] + blob->r[i] >= rows - 1)
            blob->y[i] += (blob->sY[i] * ((rows - 1 - blob->y[i]) / blob->r[i] + 0.005f));
        else if (blob->y[i] - blob->r[i] <= 0)
            blob->y[i] += (blob->sY[i] * (blob->y[i] / blob->r[i] + 0.005f));
        else
            blob->y[i] += blob->sY[i];
        // bounce x
        if (blob->x[i] < 0.01f) {
            blob->sX[i] = (float)random8(3, cols) / (256 - SEGMENT.speed);
            blob->x[i]  = 0.01f;
        } else if (blob->x[i] > (float)cols - 1.01f) {
            blob->sX[i] = (float)random8(3, cols) / (256 - SEGMENT.speed);
            blob->sX[i] = -blob->sX[i];
            blob->x[i]  = (float)cols - 1.01f;
        }
        // bounce y
        if (blob->y[i] < 0.01f) {
            blob->sY[i] = (float)random8(3, rows) / (256 - SEGMENT.speed);
            blob->y[i]  = 0.01f;
        } else if (blob->y[i] > (float)rows - 1.01f) {
            blob->sY[i] = (float)random8(3, rows) / (256 - SEGMENT.speed);
            blob->sY[i] = -blob->sY[i];
            blob->y[i]  = (float)rows - 1.01f;
        }
    }
    SEGMENT.blur(SEGMENT.custom1 >> 2);

    if (SEGENV.step < strip.now) SEGENV.step = strip.now + 2000;  // change colors every 2 seconds

    return FRAMETIME;
}
#undef MAX_BLOBS

// ---- Drift Rose ---------------------------------------------------------------
//// Drift Rose by stepko (c)2021 [https://editor.soulmatelights.com/gallery/1369-drift-rose-pattern], adapted by Blaz Kristan (AKA blazoncek)
uint16_t mode_2Ddriftrose(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    const float CX = (cols - cols % 2) / 2.f - .5f;
    const float CY = (rows - rows % 2) / 2.f - .5f;
    const float L  = min(cols, rows) / 2.f;

    SEGMENT.fadeToBlackBy(32 + (SEGMENT.speed >> 3));
    for (size_t i = 1; i < 37; i++) {
        float angle = radians(i * 10);
        // (int): the sum dips below 0 at the rim, and uint32_t of a negative float is UB (the ESP32 wraps it)
        uint32_t x = (int)((CX + (sin_t(angle) * (beatsin8_t(i, 0, L * 2) - L))) * 255.f);
        uint32_t y = (int)((CY + (cos_t(angle) * (beatsin8_t(i, 0, L * 2) - L))) * 255.f);
        SEGMENT.wu_pixel(x, y, CHSV(i * 10, 255, 255));
    }
    SEGMENT.blur(SEGMENT.intensity >> 4);

    return FRAMETIME;
}

// ---- Rotozoomer ---------------------------------------------------------------
// Plasma Rotozoomer by ldirko (c)2020 [https://editor.soulmatelights.com/gallery/457-plasma-rotozoomer], adapted for WLED by Blaz Kristan (AKA blazoncek)
uint16_t mode_2Dplasmarotozoom(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    unsigned dataSize = SEGMENT.length() + sizeof(float);
    if (!SEGENV.allocateData(dataSize)) return mode_static();  // allocation failed
    float* a      = reinterpret_cast<float*>(SEGENV.data);
    byte*  plasma = reinterpret_cast<byte*>(SEGENV.data + sizeof(float));

    unsigned ms = strip.now / 15;

    // plasma
    for (int j = 0; j < rows; j++) {
        int index = j * cols;
        for (int i = 0; i < cols; i++) {
            if (SEGMENT.check1)
                plasma[index + i] = (i * 4 ^ j * 4) + ms / 6;
            else
                plasma[index + i] = inoise8(i * 40, j * 40, ms);
        }
    }

    // rotozoom
    float f       = (sin_t(*a / 2) + ((128 - SEGMENT.intensity) / 128.0f) + 1.1f) / 1.5f;  // scale factor
    float kosinus = cos_t(*a) * f;
    float sinus   = sin_t(*a) * f;
    for (int i = 0; i < cols; i++) {
        float u1 = i * kosinus;
        float v1 = i * sinus;
        for (int j = 0; j < rows; j++) {
            byte u = abs8(u1 - j * sinus) % cols;
            byte v = abs8(v1 + j * kosinus) % rows;
            SEGMENT.setPixelColorXY(i, j, SEGMENT.color_from_palette(plasma[v * cols + u], false, PALETTE_SOLID_WRAP, 255));
        }
    }
    *a -= 0.03f + float(SEGENV.speed - 128) * 0.0002f;  // rotation speed
    if (*a < -6283.18530718f) *a += 6283.18530718f;     // 1000*2*PI, protect sin/cos from very large input float values (will give wrong results)

    return FRAMETIME;
}

// ---- Distortion Waves ---------------------------------------------------------
// Distortion waves - ldirko
// https://editor.soulmatelights.com/gallery/1089-distorsion-waves
// adapted for WLED by @blazoncek
uint16_t mode_2Ddistortionwaves(void) {
    if (!strip.isMatrix || !SEGMENT.is2D()) return mode_static();  // not a 2D set-up

    const int cols = SEGMENT.virtualWidth();
    const int rows = SEGMENT.virtualHeight();

    uint8_t speed = SEGMENT.speed / 32;
    uint8_t scale = SEGMENT.intensity / 32;

    uint8_t w = 2;

    unsigned a  = strip.now / 32;
    unsigned a2 = a / 2;
    unsigned a3 = a / 3;

    unsigned cx  = beatsin8_t(10 - speed, 0, cols - 1) * scale;
    unsigned cy  = beatsin8_t(12 - speed, 0, rows - 1) * scale;
    unsigned cx1 = beatsin8_t(13 - speed, 0, cols - 1) * scale;
    unsigned cy1 = beatsin8_t(15 - speed, 0, rows - 1) * scale;
    unsigned cx2 = beatsin8_t(17 - speed, 0, cols - 1) * scale;
    unsigned cy2 = beatsin8_t(14 - speed, 0, rows - 1) * scale;

    unsigned xoffs = 0;
    for (int x = 0; x < cols; x++) {
        xoffs += scale;
        unsigned yoffs = 0;

        for (int y = 0; y < rows; y++) {
            yoffs += scale;

            byte rdistort = cos8_t((cos8_t(((x << 3) + a) & 255) + cos8_t(((y << 3) - a2) & 255) + a3) & 255) >> 1;
            byte gdistort = cos8_t((cos8_t(((x << 3) - a2) & 255) + cos8_t(((y << 3) + a3) & 255) + a + 32) & 255) >> 1;
            byte bdistort = cos8_t((cos8_t(((x << 3) + a3) & 255) + cos8_t(((y << 3) - a) & 255) + a2 + 64) & 255) >> 1;

            byte valueR = rdistort + w * (a - (((xoffs - cx) * (xoffs - cx) + (yoffs - cy) * (yoffs - cy)) >> 7));
            byte valueG = gdistort + w * (a2 - (((xoffs - cx1) * (xoffs - cx1) + (yoffs - cy1) * (yoffs - cy1)) >> 7));
            byte valueB = bdistort + w * (a3 - (((xoffs - cx2) * (xoffs - cx2) + (yoffs - cy2) * (yoffs - cy2)) >> 7));

            valueR = gamma8(cos8_t(valueR));
            valueG = gamma8(cos8_t(valueG));
            valueB = gamma8(cos8_t(valueB));

            SEGMENT.setPixelColorXY(x, y, RGBW32(valueR, valueG, valueB, 0));
        }
    }

    return FRAMETIME;
}

#undef XY

}  // namespace wled
