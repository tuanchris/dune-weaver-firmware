// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "ThrTranslator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Kinematics {
    namespace {
        const float TWO_PI = 2.0f * float(M_PI);
    }

    float positive_mod_2pi(float angle) {
        float m = fmodf(angle, TWO_PI);
        if (m < 0.0f) {
            m += TWO_PI;
        }
        return m;
    }

    ThrLine ThrTranslator::translate(const char* line, char* out, size_t outlen) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        // Blank lines and "# comment" lines
        if (*p == '\0' || *p == '#') {
            return ThrLine::Skip;
        }

        char* end   = const_cast<char*>(p);
        float theta = strtof(p, &end);
        if (end == p) {
            return ThrLine::Invalid;
        }
        p         = end;
        float rho = strtof(p, &end);
        if (end == p) {
            return ThrLine::Invalid;
        }

        bool rho_only = false;
        if (_in_preamble) {
            if (theta == 0.0f && rho == 0.0f) {
                // Leading "0 0" pairs mean "start from the center": move rho
                // to 0 without spinning theta back to absolute zero.
                rho_only = true;
            } else {
                _in_preamble = false;
            }
        }

        if (!rho_only && !_offset_locked) {
            // Remove the pattern's leading whole revolutions so its first
            // real coordinate lands in [0, 2pi).
            _theta_offset  = theta - positive_mod_2pi(theta);
            _offset_locked = true;
        }

        if (rho < 0.0f) {
            rho = 0.0f;
        } else if (rho > 1.0f) {
            rho = 1.0f;
        }

        char buf[64];
        int  n;
        if (rho_only) {
            n = snprintf(buf, sizeof(buf), "G90G1Y%.5f", rho);
        } else {
            n = snprintf(buf, sizeof(buf), "G90G1X%.5fY%.5f", theta - _theta_offset, rho);
        }
        bool emit_feed = _feed > 0.0f && _feed != _emitted_feed;
        if (n > 0 && size_t(n) < sizeof(buf) && emit_feed) {
            n += snprintf(buf + n, sizeof(buf) - n, "F%.1f", _feed);
        }
        if (n <= 0 || size_t(n) >= sizeof(buf) || size_t(n) >= outlen) {
            return ThrLine::Oversize;
        }

        for (int i = 0; i <= n; i++) {
            out[i] = buf[i];
        }
        if (emit_feed) {
            _emitted_feed = _feed;
        }
        return ThrLine::Move;
    }
}
