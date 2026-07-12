// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "PlaylistParse.h"

#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

namespace PlaylistParse {

    std::vector<std::string> parse_playlist(const std::string& content, size_t max_items) {
        std::vector<std::string> items;
        size_t                   pos = 0;
        while (pos < content.size() && items.size() < max_items) {
            size_t      eol  = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos              = eol == std::string::npos ? content.size() : eol + 1;

            size_t comment = line.find('#');
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos) {
                continue;
            }
            size_t e = line.find_last_not_of(" \t\r");
            line     = line.substr(b, e - b + 1);
            if (line[0] != '/') {
                line = "/" + line;
            }
            items.push_back(line);
        }
        return items;
    }

    float first_rho(const std::string& content) {
        float coords[3][2];
        int   found = 0;
        size_t pos  = 0;
        while (found < 3 && pos < content.size()) {
            size_t      eol  = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos              = eol == std::string::npos ? content.size() : eol + 1;

            if (line.empty() || line[0] == '#') {
                continue;
            }
            float t, r;
            if (sscanf(line.c_str(), "%f %f", &t, &r) == 2) {
                coords[found][0] = t;
                coords[found][1] = r;
                found++;
            }
        }
        if (found == 0) {
            return -1.0f;
        }
        auto isZero = [](float* c) { return c[0] > -1e-9f && c[0] < 1e-9f && c[1] > -1e-9f && c[1] < 1e-9f; };
        if (found >= 3 && isZero(coords[0]) && isZero(coords[1])) {
            return coords[2][1];
        }
        return coords[0][1];
    }

    Clear choose_clear(int clear_mode, float first_rho, uint32_t rnd) {
        const Clear randomized[3] = { Clear::FromIn, Clear::FromOut, Clear::Sideway };
        switch (clear_mode) {
            case CLEAR_IN:
                return Clear::FromIn;
            case CLEAR_OUT:
                return Clear::FromOut;
            case CLEAR_SIDEWAY:
                return Clear::Sideway;
            case CLEAR_RANDOM:
                return randomized[rnd % 3];
            case CLEAR_ADAPTIVE:
                if (first_rho < 0.0f) {
                    // Unknown start; match dune-weaver's fallback of picking randomly
                    return randomized[rnd % 3];
                }
                return first_rho < 0.5f ? Clear::FromOut : Clear::FromIn;
            default:
                return Clear::None;
        }
    }

    bool parse_clear_mode(const char* name, int& mode) {
        if (!name || !*name) {
            return false;
        }
        struct {
            const char* key;
            int         value;
        } table[] = {
            { "none", CLEAR_NONE },     { "adaptive", CLEAR_ADAPTIVE }, { "in", CLEAR_IN },
            { "out", CLEAR_OUT },       { "sideway", CLEAR_SIDEWAY },   { "side", CLEAR_SIDEWAY },
            { "random", CLEAR_RANDOM },
        };
        for (auto const& e : table) {
            if (strcasecmp(name, e.key) == 0) {
                mode = e.value;
                return true;
            }
        }
        return false;
    }

    bool parse_run_args(const std::string& value, std::string& path, std::string& clear_mode) {
        path.clear();
        clear_mode.clear();

        size_t end = value.find_last_not_of(" \t\r");
        if (end == std::string::npos) {
            return false;
        }
        size_t begin = value.find_first_not_of(" \t\r");

        size_t last_ws = value.find_last_of(" \t", end);
        size_t tok     = last_ws == std::string::npos || last_ws < begin ? begin : last_ws + 1;
        if (value.compare(tok, 6, "clear=") == 0) {
            clear_mode = value.substr(tok + 6, end - (tok + 6) + 1);
            if (tok == begin) {
                return false;  // "clear=<mode>" with no path
            }
            end = value.find_last_not_of(" \t\r", last_ws);
        }
        path = value.substr(begin, end - begin + 1);
        return true;
    }
}
