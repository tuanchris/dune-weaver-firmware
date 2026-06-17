// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "SandStatus.h"

#include <cstdio>
#include <cstdlib>

namespace SandStatus {
    namespace {
        // Append a float with the given precision, no trailing junk.
        void append_float(std::string& out, float v, int prec) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%.*f", prec, v);
            out += buf;
        }

        void append_int(std::string& out, int v) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", v);
            out += buf;
        }

        void member_str(std::string& out, const char* key, const char* value) {
            out += '"';
            out += key;
            out += "\":";
            append_escaped(out, value);
        }
    }

    void append_escaped(std::string& out, const char* s) {
        out += '"';
        for (; *s; ++s) {
            char c = *s;
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char u[8];
                        snprintf(u, sizeof(u), "\\u%04x", c);
                        out += u;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        out += '"';
    }

    std::string encode(const Data& d) {
        std::string o;
        o.reserve(256);
        o += '{';

        member_str(o, "state", d.state);
        o += ",\"theta\":";
        append_float(o, d.theta, 4);
        o += ",\"rho\":";
        append_float(o, d.rho, 4);
        o += ",\"feed\":";
        append_float(o, d.feed, 0);
        o += ",\"feed_override\":";
        append_int(o, d.feed_override);
        o += ",\"running\":";
        o += d.running ? "true" : "false";
        o += ",\"file\":";
        append_escaped(o, d.file.c_str());
        o += ",\"progress\":";
        append_float(o, d.progress, 1);

        o += ",\"playlist\":{";
        o += "\"active\":";
        o += d.playlist_active ? "true" : "false";
        o += ",\"index\":";
        append_int(o, d.playlist_index);
        o += ",\"total\":";
        append_int(o, d.playlist_total);
        o += ',';
        member_str(o, "name", d.playlist_name.c_str());
        o += ",\"clearing\":";
        o += d.playlist_clearing ? "true" : "false";
        o += ",\"quiet\":";
        o += d.quiet ? "true" : "false";
        o += '}';

        if (d.has_led) {
            o += ",\"led\":{";
            member_str(o, "effect", d.led_effect);
            o += ",\"brightness\":";
            append_int(o, d.led_brightness);
            o += '}';
        }

        o += '}';
        return o;
    }

    std::string encode_array(const std::vector<std::string>& items) {
        std::string o;
        o += '[';
        for (size_t i = 0; i < items.size(); i++) {
            if (i) {
                o += ',';
            }
            append_escaped(o, items[i].c_str());
        }
        o += ']';
        return o;
    }

    std::string encode_object(const std::vector<std::pair<std::string, std::string>>& items) {
        std::string o;
        o += '{';
        for (size_t i = 0; i < items.size(); i++) {
            if (i) {
                o += ',';
            }
            append_escaped(o, items[i].first.c_str());
            o += ':';
            append_escaped(o, items[i].second.c_str());
        }
        o += '}';
        return o;
    }

    float parse_sd_percent(const std::string& progress) {
        // "SD:<percent>,<path>"; anything else (e.g. "SD: name: Sent" or
        // "") has no parseable percent.
        if (progress.rfind("SD:", 0) != 0) {
            return -1.0f;
        }
        const char* p = progress.c_str() + 3;
        if (*p < '0' || *p > '9') {
            return -1.0f;
        }
        char* end = nullptr;
        float v   = strtof(p, &end);
        if (end == p) {
            return -1.0f;
        }
        return v;
    }
}
