// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <cstdint>

/*
  Total time the machine has spent in Hold, so a pattern's reported `elapsed`
  can leave its pauses out.  Without it `elapsed` kept counting through a
  pause, and a client's ETA (elapsed * (1 - progress) / progress) came out
  long by the paused time scaled up by (1 - progress) / progress.

  set_state() feeds every state change in; a job notes held() when it starts
  and subtracts what accrued since.  Std-only so it is unit-testable in
  [env:tests]; times are caller-supplied microseconds.
*/
struct HoldClock {
    int64_t total = 0;   // finished holds
    int64_t since = -1;  // when the current hold began; -1 = not held

    void set_held(bool held, int64_t now) {
        if (held && since < 0) {
            since = now;
        } else if (!held && since >= 0) {
            total += now - since;
            since = -1;
        }
    }

    int64_t held(int64_t now) const { return total + (since >= 0 ? now - since : 0); }
};
