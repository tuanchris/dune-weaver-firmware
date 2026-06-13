// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "QuietHours.h"

#include <cstdio>

namespace QuietHours {
    namespace {
        const char* const dayNames[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

        // strips surrounding spaces/tabs
        std::string trim(const std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) {
                return "";
            }
            size_t e = s.find_last_not_of(" \t");
            return s.substr(b, e - b + 1);
        }

        bool parseTime(const std::string& s, uint16_t& minutes) {
            unsigned h, m;
            if (sscanf(s.c_str(), "%u:%u", &h, &m) != 2 || h > 23 || m > 59) {
                return false;
            }
            minutes = static_cast<uint16_t>(h * 60 + m);
            return true;
        }

        bool parseDays(const std::string& s, uint8_t& days) {
            if (s == "daily") {
                days = 0x7F;
                return true;
            }
            if (s == "weekdays") {
                days = 0x3E;  // mon..fri
                return true;
            }
            if (s == "weekends") {
                days = 0x41;  // sun, sat
                return true;
            }
            days       = 0;
            size_t pos = 0;
            while (pos <= s.size()) {
                size_t      sep  = s.find('+', pos);
                std::string name = trim(s.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos));
                pos              = sep == std::string::npos ? s.size() + 1 : sep + 1;
                bool found       = false;
                for (int d = 0; d < 7; d++) {
                    if (name == dayNames[d]) {
                        days |= 1 << d;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return false;
                }
            }
            return days != 0;
        }
    }

    std::vector<Slot> parse(const std::string& spec, std::string* err) {
        std::vector<Slot> slots;
        size_t            pos = 0;
        while (pos < spec.size()) {
            size_t      sep  = spec.find(',', pos);
            std::string item = trim(spec.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos));
            pos              = sep == std::string::npos ? spec.size() : sep + 1;
            if (item.empty()) {
                continue;
            }

            Slot   slot;
            size_t dash = item.find('-');
            size_t at   = item.find('@');
            if (dash == std::string::npos || at == std::string::npos || at < dash) {
                if (err) {
                    *err = "expected HH:MM-HH:MM@days in '" + item + "'";
                }
                return {};
            }
            if (!parseTime(trim(item.substr(0, dash)), slot.start_min) ||
                !parseTime(trim(item.substr(dash + 1, at - dash - 1)), slot.end_min)) {
                if (err) {
                    *err = "bad time in '" + item + "'";
                }
                return {};
            }
            if (!parseDays(trim(item.substr(at + 1)), slot.days)) {
                if (err) {
                    *err = "bad day spec in '" + item + "'";
                }
                return {};
            }
            slots.push_back(slot);
        }
        return slots;
    }

    bool match(const std::vector<Slot>& slots, int wday, int minute_of_day) {
        if (wday < 0 || wday > 6) {
            return false;
        }
        for (auto const& slot : slots) {
            if (!(slot.days & (1 << wday))) {
                continue;
            }
            if (slot.start_min <= slot.end_min) {
                if (minute_of_day >= slot.start_min && minute_of_day <= slot.end_min) {
                    return true;
                }
            } else {
                // Spans midnight
                if (minute_of_day >= slot.start_min || minute_of_day <= slot.end_min) {
                    return true;
                }
            }
        }
        return false;
    }
}
