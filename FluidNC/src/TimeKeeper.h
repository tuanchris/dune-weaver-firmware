// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// Small wall-clock API over the TimeKeeper module, so other modules (e.g. the
// Sand HTTP API) can read/sync the clock without depending on the module class.

#pragma once

#include <ctime>
#include <cstddef>

namespace Clock {
    bool        isSet();                          // clock plausibly set (NTP or manual)
    time_t      now();                            // current unix epoch seconds
    void        localString(char* buf, size_t n); // "YYYY-MM-DD HH:MM:SS" in the active TZ
    const char* tz();                             // effective POSIX TZ string

    // Manually set the clock (app auto-sync / AP mode).  Rejects implausibly old
    // epochs (< 2023).  Returns true on success.
    bool setEpoch(time_t t);

    // Apply + persist a POSIX TZ string at runtime (e.g. "ICT-7").  Returns
    // false if no TimeKeeper is configured.
    bool setTz(const char* tz);
}
