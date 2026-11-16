// Copyright (c) 2026 - Dune Weaver
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

// Keeping /patterns/index.json (the prebuilt catalog GET /sand_patterns serves
// verbatim) in step with uploads: an uploaded pattern is appended to it in place
// (its final "]" becomes ',"<key>"]'), so a file sent while the app is closed (a
// background sync) still shows in every client's list. Never a parse or rewrite
// of the whole file: it is ~20 KB on a full card and the heap is tight.
// Std-only, unit-tested (FluidNC/tests/PatternManifestTest.cpp).

#include <cstddef>
#include <cstring>
#include <string>

namespace PatternManifest {
    inline bool endsWithNoCase(const std::string& s, const char* suffix) {
        size_t n = strlen(suffix);
        if (s.size() < n) {
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            char a = s[s.size() - n + i], b = suffix[i];
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (a != b) {
                return false;
            }
        }
        return true;
    }

    // The catalog entry for an uploaded file: its path under /patterns
    // ("/sd/patterns/a/b.thr" -> "a/b.thr"), or "" when the catalog doesn't list
    // it (outside /patterns, not a pattern, index.json itself, the previews bundle).
    inline std::string keyFor(const std::string& sdPath) {
        static const char* const roots[] = { "/sd/patterns/", "/patterns/" };
        std::string              key;
        for (const char* root : roots) {
            if (sdPath.compare(0, strlen(root), root) == 0) {
                key = sdPath.substr(strlen(root));
                break;
            }
        }
        if (key.empty() || key.compare(0, 9, "previews/") == 0) {
            return "";
        }
        if (!(endsWithNoCase(key, ".thr") || endsWithNoCase(key, ".gcode") || endsWithNoCase(key, ".gc") || endsWithNoCase(key, ".nc"))) {
            return "";
        }
        return key;
    }

    // `key` as a JSON string literal.
    inline std::string quoted(const std::string& key) {
        std::string out = "\"";
        for (unsigned char c : key) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += char(c);
            } else if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += char(c);
            }
        }
        return out + "\"";
    }

    inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

    // Where and what to write to append `quotedKey`, given the catalog's last
    // bytes `tail` (they start at file offset `tailAt`): `text` goes at offset
    // `at`, over the closing "]" and any whitespace after it. false = the tail
    // doesn't end a JSON array the way a catalog does (leave the file alone).
    inline bool append(const std::string& tail, size_t tailAt, const std::string& quotedKey, size_t& at, std::string& text) {
        size_t end = tail.size();
        while (end && isSpace(tail[end - 1])) {
            --end;
        }
        if (!end || tail[end - 1] != ']') {
            return false;
        }
        size_t close = end - 1;
        size_t prev  = close;
        while (prev && isSpace(tail[prev - 1])) {
            --prev;
        }
        if (!prev) {
            return false;  // can't see what precedes "]": not enough tail
        }
        char before = tail[prev - 1];
        if (before == '[') {
            text = quotedKey + "]";
        } else if (before == '"') {
            text = "," + quotedKey + "]";
        } else {
            return false;
        }
        if (text.size() < tail.size() - close) {
            return false;  // more trailing bytes than the write would cover
        }
        at = tailAt + close;
        return true;
    }

    // Whether `needle` occurs in the stream read(buf, max) -> bytes (0 at the
    // end), searched in `cap`-byte windows that overlap by needle.size() - 1.
    template <class Read>
    bool contains(Read read, const std::string& needle, char* buf, size_t cap) {
        size_t n = needle.size();
        if (n == 0 || cap < n) {
            return false;
        }
        size_t have = 0;
        while (true) {
            size_t got = read(buf + have, cap - have);
            if (got == 0) {
                return false;
            }
            have += got;
            for (size_t i = 0; i + n <= have; ++i) {
                if (buf[i] == needle[0] && memcmp(buf + i, needle.data(), n) == 0) {
                    return true;
                }
            }
            size_t keep = n - 1 < have ? n - 1 : have;
            memmove(buf, buf + have - keep, keep);
            have = keep;
        }
    }
}
