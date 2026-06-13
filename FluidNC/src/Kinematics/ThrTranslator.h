// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  ThrTranslator converts ".thr" pattern lines ("<theta> <rho>") into
  G-code move lines, holding the small per-job state that makes the
  translation stateful:

   - leading "0 0" preamble pairs become rho-only moves (start from
     the center without unwinding theta),
   - the first real coordinate locks a whole-revolution offset so the
     pattern starts within [0, 2pi) instead of spinning down from an
     accumulated theta,
   - an emitted move carries an F word whenever the feed value has
     changed since the last emitted F (including the first move), so a
     live feed setting takes effect mid-pattern within one move.

  This class is dependency-free (std only) so it can be unit-tested in
  the native test environment; ThetaRho owns one and layers job/channel
  tracking and logging on top.
*/

#include <cstddef>
#include <cstdint>

namespace Kinematics {

    // positive remainder of angle / 2pi, in [0, 2pi)
    float positive_mod_2pi(float angle);

    enum class ThrLine : uint8_t {
        Skip,      // blank or comment; nothing to execute
        Invalid,   // not a "<theta> <rho>" pair
        Oversize,  // translated move did not fit the output buffer
        Move,      // out contains a G-code move line
    };

    class ThrTranslator {
    public:
        // Begin a new job.  feed > 0 puts F on the first move.
        void start(float feed) {
            _feed          = feed;
            _emitted_feed  = 0.0f;
            _in_preamble   = true;
            _offset_locked = false;
            _theta_offset  = 0.0f;
        }

        // Update the feed; the next emitted move carries F if it changed.
        // A feed of 0 keeps the last emitted (modal) feed.
        void set_feed(float feed) { _feed = feed; }

        // Translate one source line into outlen-byte buffer out.
        ThrLine translate(const char* line, char* out, size_t outlen);

        bool  offset_locked() const { return _offset_locked; }
        float theta_offset() const { return _theta_offset; }

    private:
        float _feed          = 0.0f;  // requested feed, mm/min in motor space
        float _emitted_feed  = 0.0f;  // last feed actually emitted as an F word
        bool  _in_preamble   = false;
        bool  _offset_locked = false;
        float _theta_offset  = 0.0f;
    };
}
