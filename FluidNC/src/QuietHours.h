// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Quiet-hours ("Still Sands") slot parsing and matching, separated from
  time sources and machine state so it can be unit-tested natively.

  Slot specification syntax (the $Sands/Slots setting):

      09:00-20:00@daily,22:30-06:00@weekends,13:00-14:00@mon+tue+fri

  Comma-separated slots; each is HH:MM-HH:MM@<days> where <days> is
  daily, weekdays, weekends, or '+'-joined day names (sun..sat).
  A window whose end is before its start spans midnight.  Matching the
  day follows dune-weaver semantics: the day filter applies to the
  CURRENT day, so the after-midnight tail of a spanning slot matches
  only when the current (new) day is in the day set.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace QuietHours {

    struct Slot {
        uint16_t start_min;  // minutes from midnight, 0..1439
        uint16_t end_min;
        uint8_t  days;  // bit 0 = Sunday ... bit 6 = Saturday (tm_wday)
    };

    // Parse a slot specification.  On error returns an empty vector and,
    // when err is non-null, a message describing the first problem.
    std::vector<Slot> parse(const std::string& spec, std::string* err = nullptr);

    // True when the time (tm_wday 0..6 with 0=Sunday, minutes from
    // midnight) falls inside any slot.  Boundaries are inclusive.
    bool match(const std::vector<Slot>& slots, int wday, int minute_of_day);
}
