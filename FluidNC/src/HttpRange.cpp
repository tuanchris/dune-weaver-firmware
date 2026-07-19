// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "HttpRange.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace HttpRange {
    // Parse a decimal number at *p, advancing p past the digits.  Returns
    // false if there are no digits.  Saturates instead of wrapping so a
    // pathological 30-digit value can't alias into a valid offset.
    static bool parseNum(const char*& p, size_t& out) {
        if (!isdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
        size_t v = 0;
        while (isdigit(static_cast<unsigned char>(*p))) {
            size_t digit = static_cast<size_t>(*p - '0');
            if (v > (SIZE_MAX - digit) / 10) {
                v = SIZE_MAX;  // saturate; caller range-checks against fileSize
            } else {
                v = v * 10 + digit;
            }
            ++p;
        }
        out = v;
        return true;
    }

    Result parse(const char* value, size_t fileSize, size_t& start, size_t& len) {
        if (!value || strncmp(value, "bytes=", 6) != 0) {
            return Result::None;
        }
        const char* p = value + 6;
        if (strchr(p, ',')) {
            return Result::None;  // multi-range: legal to ignore, serve whole
        }

        if (*p == '-') {
            // Suffix form "-n": the final n bytes.
            ++p;
            size_t n;
            if (!parseNum(p, n) || *p != '\0') {
                return Result::None;
            }
            if (n == 0 || fileSize == 0) {
                return Result::Unsatisfiable;
            }
            len   = n < fileSize ? n : fileSize;
            start = fileSize - len;
            return Result::Partial;
        }

        size_t first;
        if (!parseNum(p, first) || *p != '-') {
            return Result::None;
        }
        ++p;

        if (*p == '\0') {
            // Open form "a-": from a to EOF.
            if (first >= fileSize) {
                return Result::Unsatisfiable;
            }
            start = first;
            len   = fileSize - first;
            return Result::Partial;
        }

        // Closed form "a-b".
        size_t last;
        if (!parseNum(p, last) || *p != '\0' || last < first) {
            return Result::None;
        }
        if (first >= fileSize) {
            return Result::Unsatisfiable;
        }
        if (last >= fileSize) {
            last = fileSize - 1;
        }
        start = first;
        len   = last - first + 1;
        return Result::Partial;
    }
}
