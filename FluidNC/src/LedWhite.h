// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  RGBW strip support for Leds: the wire byte order (incl. an optional W slot)
  and WLED's auto-white calculation.

  color_order is a permutation of R, G, B with an optional W anywhere, e.g.
  "GRB" (WS2812) or "GRBW" (SK6812 RGBW).  Four letters = 4 bytes per pixel.

  The effects render 24-bit RGB.  On a 4-byte strip the W byte is derived per
  pixel exactly as WLED's Bus::autoWhiteCalc does it ($LED/White):
    none     W = 0                           (WLED "manual only"; white die unused)
    brighter W = min(R,G,B), RGB untouched   (WLED "brighter")
    accurate W = min(R,G,B), RGB -= W        (WLED "accurate": color-true, W carries the white)
    max      W = max(R,G,B), RGB untouched   (WLED "max")
  WLED's "dual" is brighter-with-a-manual-white-slider; we have no slider, so it
  is not offered.

  Std-only so it is unit-tested in [env:tests]; Leds owns the state.
*/

#include <cstdint>
#include <cstddef>

class LedWhite {
public:
    enum Mode : int {
        None     = 0,
        Brighter = 1,
        Accurate = 2,
        Max      = 3,
    };

    struct Order {
        int  bpp = 3;               // bytes per pixel on the wire
        int  ri = 1, gi = 0, bi = 2;  // wire offsets of R, G, B (GRB default)
        int  wi = -1;               // wire offset of W, or -1 on an RGB strip
        bool rgbw() const { return wi >= 0; }
    };

    // Parse a color_order string.  Returns false (and leaves `out` as GRB) when
    // it is not a permutation of RGB with at most one W.
    static bool parseOrder(const char* s, Order& out) {
        Order o;
        o.ri = o.gi = o.bi = o.wi = -1;
        int n = 0;
        for (; s && s[n]; n++) {
            if (n >= 4) {
                return false;
            }
            int* slot = nullptr;
            switch (s[n]) {
                case 'R': case 'r': slot = &o.ri; break;
                case 'G': case 'g': slot = &o.gi; break;
                case 'B': case 'b': slot = &o.bi; break;
                case 'W': case 'w': slot = &o.wi; break;
                default: return false;
            }
            if (*slot >= 0) {
                return false;  // repeated letter
            }
            *slot = n;
        }
        if (o.ri < 0 || o.gi < 0 || o.bi < 0) {
            return false;
        }
        o.bpp = o.wi >= 0 ? 4 : 3;
        if (n != o.bpp) {
            return false;
        }
        out = o;
        return true;
    }

    // WLED Bus::autoWhiteCalc for a 24-bit color with no manual W.
    static void autoWhite(Mode mode, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& w) {
        switch (mode) {
            case Max:
                w = r > g ? (r > b ? r : b) : (g > b ? g : b);
                break;
            case Brighter:
            case Accurate:
                w = r < g ? (r < b ? r : b) : (g < b ? g : b);
                if (mode == Accurate) {
                    r -= w;
                    g -= w;
                    b -= w;
                }
                break;
            case None:
            default:
                w = 0;
                break;
        }
    }

    // Write one pixel into a wire-order buffer, brightness-scaled (scale is
    // brightness+1 so 255 passes the value through unchanged).
    static void pack(const Order& o, Mode mode, uint8_t* p, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
        uint16_t scale = uint16_t(brightness) + 1;
        uint8_t  w     = 0;
        if (o.rgbw()) {
            autoWhite(mode, r, g, b, w);
            p[o.wi] = uint8_t((w * scale) >> 8);
        }
        p[o.ri] = uint8_t((r * scale) >> 8);
        p[o.gi] = uint8_t((g * scale) >> 8);
        p[o.bi] = uint8_t((b * scale) >> 8);
    }
};
