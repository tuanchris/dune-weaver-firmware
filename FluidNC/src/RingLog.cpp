// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "RingLog.h"

#include <Arduino.h>  // millis()
#include <cstdio>
#include <mutex>

namespace {
    // 8 KB holds roughly the last 100-150 log lines - enough to see why a
    // day-long playlist ended without competing with the heap the log exists
    // to watch.
    constexpr size_t kCap = 8192;

    char       _buf[kCap];
    size_t     _head  = 0;  // next write index
    size_t     _count = 0;  // bytes stored, saturates at kCap
    std::mutex _mu;

    // Named ringPush, NOT push: inside RingLog::print_msg an unqualified
    // push() resolves to the inherited Channel::push(uint8_t) (member lookup
    // hides namespace scope), which feeds the never-drained input queue --
    // that leak boot-looped a board by exhausting the heap within a minute.
    void ringPush(char c) {
        _buf[_head] = c;
        _head       = (_head + 1) % kCap;
        if (_count < kCap) {
            ++_count;
        }
    }
}

void RingLog::print_msg(MsgLevel level, const char* msg) {
    if (_message_level < level) {
        return;
    }
    char prefix[16];
    int  n = snprintf(prefix, sizeof(prefix), "[+%lu] ", millis() / 1000UL);

    std::lock_guard<std::mutex> lock(_mu);
    for (int i = 0; i < n; ++i) {
        ringPush(prefix[i]);
    }
    for (const char* p = msg; *p; ++p) {
        ringPush(*p);
    }
    ringPush('\n');
}

void RingLog::dump(std::string& out) {
    std::lock_guard<std::mutex> lock(_mu);
    out.reserve(out.size() + _count);
    size_t start = (_head + kCap - _count) % kCap;
    size_t i     = 0;
    // After a wrap the oldest entry is usually a partial line; skip to the
    // first boundary so the dump starts clean.
    if (_count == kCap) {
        while (i < _count && _buf[(start + i) % kCap] != '\n') {
            ++i;
        }
        ++i;
    }
    for (; i < _count; ++i) {
        out += _buf[(start + i) % kCap];
    }
}

RingLog ringLog;
