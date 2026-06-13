// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Pure JSON model for the sand-table status API, separated from machine
  state and channel I/O so it can be unit-tested natively.  SandApi.cpp
  gathers live state into a Data struct and writes encode(Data) to the
  requesting channel; the same JSON is served over serial, the
  websocket, and the existing /command HTTP endpoint.

  The document is a compact single-line JSON object, e.g.

    {"state":"Run","theta":1.2340,"rho":0.5000,"feed":100,"running":true,
     "file":"/sd/star.thr","progress":42.5,
     "playlist":{"active":true,"index":2,"total":10,"name":"evening",
                 "clearing":false,"quiet":false},
     "led":{"effect":"rainbow","brightness":40}}
*/

#include <string>
#include <vector>

namespace SandStatus {

    struct Data {
        const char* state = "Unknown";  // GRBL state name

        float theta = 0.0f;  // radians (ThetaRho cartesian X)
        float rho   = 0.0f;  // 0..1    (ThetaRho cartesian Y)
        float feed  = 0.0f;  // $THR/Feed, motor mm/min

        bool        running  = false;  // a file job is active
        std::string file;              // current job file, empty if none
        float       progress = -1.0f;  // 0..100, or <0 if unknown

        bool        playlist_active   = false;
        int         playlist_index    = 0;  // 0-based
        int         playlist_total    = 0;
        std::string playlist_name;
        bool        playlist_clearing = false;
        bool        quiet             = false;  // Still Sands active

        bool        has_led        = false;
        const char* led_effect     = "off";
        int         led_brightness = 0;
    };

    // Append s to out as a JSON string literal, escaping as needed.
    void append_escaped(std::string& out, const char* s);

    // Compact single-line JSON object for the status.
    std::string encode(const Data& d);

    // JSON array of strings, e.g. ["/a.thr","/b.thr"].
    std::string encode_array(const std::vector<std::string>& items);

    // Percent from an InputFile progress string ("SD:<pct>,<path>");
    // returns -1 when the string is empty or not a percent report.
    float parse_sd_percent(const std::string& progress);
}
