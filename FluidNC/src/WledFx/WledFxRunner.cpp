// See WledFxRunner.h.  Mirrored by dune-weaver-mobile/src/lib/wledfx/runner.ts.
#include "WledFxRunner.h"
#include "WledFx.h"
#include "../LedPalettes.h"

#include <new>

namespace wled {

// FastLED's PartyColors_p: what WLED's Default palette means where an effect
// asks for palette colors (color_from_palette with mcol > 2, SEGPALETTE).
static const uint32_t kParty[16] = { 0x5500AB, 0x84007C, 0xB5004B, 0xE5001B, 0xE81700, 0xB84700, 0xAB7700, 0xABAB00,
                                     0xAB5500, 0xDD2200, 0xF2000E, 0xC2003E, 0x8F0071, 0x5F00A1, 0x2F00D0, 0x0007F9 };

void seed(uint16_t s) {
    rand16seed = s;
}

void Runner::release() {
    if (_seg) {
        _seg->deallocateData();
        delete[] _seg->_px;
        delete _seg;
        _seg = nullptr;
    }
    _fx = nullptr;
    _n  = 0;
}

// Bilinear sample of the matrix at (x, y), 8.8 fixed point.
static void sample(const uint32_t* px, int x256, int y256, uint8_t* out) {
    const int lim = (kMatrix - 1) * 256;
    x256          = x256 < 0 ? 0 : x256 > lim ? lim : x256;
    y256          = y256 < 0 ? 0 : y256 > lim ? lim : y256;
    int x0 = x256 >> 8, y0 = y256 >> 8, fx = x256 & 255, fy = y256 & 255;
    int x1 = x0 + 1 < kMatrix ? x0 + 1 : x0, y1 = y0 + 1 < kMatrix ? y0 + 1 : y0;
    uint32_t c00 = px[x0 + y0 * kMatrix], c10 = px[x1 + y0 * kMatrix];
    uint32_t c01 = px[x0 + y1 * kMatrix], c11 = px[x1 + y1 * kMatrix];
    for (int k = 0, sh = 16; k < 3; k++, sh -= 8) {
        uint32_t v = ((c00 >> sh) & 255) * (256 - fx) * (256 - fy) + ((c10 >> sh) & 255) * fx * (256 - fy) +
                     ((c01 >> sh) & 255) * (256 - fx) * fy + ((c11 >> sh) & 255) * fx * fy;
        out[k] = v >> 16;
    }
}

// The strip, as points on the matrix: a ring on its inscribed circle, a
// rectangle on its border; both from 12 o'clock, clockwise (as the app draws
// them).  ponytail: a square matrix, so a long rectangle's sides stretch.
static void mapToStrip(const uint32_t* px, bool ring, uint8_t* rgb, int n) {
    const int c = (kMatrix - 1) * 128;  // centre, and the ring's radius, 8.8
    const int side = (kMatrix - 1) * 256;
    for (int i = 0; i < n; i++) {
        int x, y;
        if (ring) {
            uint16_t t = (uint32_t)i * 65536 / n;
            x          = c + (c * sin16_t(t)) / 32767;
            y          = c - (c * cos16_t(t)) / 32767;
        } else {
            int s = (int)(((int64_t)i * side * 4) / n) + side / 2;  // distance along the border from the top-left corner
            s %= side * 4;
            if (s < side) {
                x = s, y = 0;
            } else if (s < 2 * side) {
                x = side, y = s - side;
            } else if (s < 3 * side) {
                x = 3 * side - s, y = side;
            } else {
                x = 0, y = 4 * side - s;
            }
        }
        sample(px, x, y, rgb + 3 * i);
    }
}

bool Runner::render(const FxInfo& fx, const FxFrame& f, uint8_t* rgb, int n) {
    if (&fx != _fx || n != _n) {
        release();
        int len = fx.is2D ? kMatrix * kMatrix : n;
        _seg    = new (std::nothrow) Segment();
        if (!_seg) {
            return false;
        }
        _seg->_px = new (std::nothrow) uint32_t[len]();
        if (!_seg->_px) {
            delete _seg;
            _seg = nullptr;
            return false;
        }
        _seg->_w = fx.is2D ? kMatrix : n;
        _seg->_h = fx.is2D ? kMatrix : 1;
        _fx      = &fx;
        _n       = n;
    }
    Segment& seg  = *_seg;
    seg.speed     = f.speed;
    seg.intensity = f.intensity;
    seg.custom1   = fx.c1;
    seg.custom2   = fx.c2;
    seg.custom3   = fx.c3;
    seg.check1    = fx.checks & 1;
    seg.check2    = fx.checks & 2;
    seg.check3    = fx.checks & 4;
    seg.colors[0] = f.color1 & 0xFFFFFF;
    seg.colors[1] = f.color2 & 0xFFFFFF;
    seg.colors[2] = 0;
    seg.palette   = f.palette < LedPalettes::kCount ? f.palette : 0;
    seg._currentPalette = CRGBPalette16(seg.palette == 0 ? kParty : LedPalettes::kTables[seg.palette - 1]);

    ::wled::_seg   = &seg;
    strip.now      = f.now;
    strip.isMatrix = fx.is2D;
    now_ms         = f.now;
    // WLED's WS2812FX::service(): run the effect once its last delay is up.
    if (f.now > seg.next_time || seg.call == 0) {
        uint16_t delay = fx.fn();
        seg.next_time  = f.now + delay;
        seg.call++;
    }

    if (fx.is2D) {
        mapToStrip(seg._px, f.ring, rgb, n);
    } else {
        for (int i = 0; i < n; i++) {
            uint32_t c     = seg._px[i];
            rgb[3 * i]     = c >> 16;
            rgb[3 * i + 1] = c >> 8;
            rgb[3 * i + 2] = c;
        }
    }
    return true;
}

}  // namespace wled
