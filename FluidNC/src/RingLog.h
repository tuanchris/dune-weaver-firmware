// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Channel.h"

#include <string>

// Rolling capture of every formatted log line, retrievable over HTTP
// (/sand_log).  On this headless build serial is unattached and the WebSocket
// channel is disabled, so without this the runtime log (playlist finish
// reasons, SD errors, alarms) is emitted into the void.  Complements
// StartupLog, which only covers boot: this ring holds the most recent lines
// from the whole session, each prefixed with uptime seconds.
//
// Registered in allChannels for the lifetime of the process; lines arrive via
// the virtual print_msg fan-out (usually from the output task, but early-boot
// and fallback paths call from other tasks, hence the internal lock).
class RingLog : public Channel {
public:
    RingLog() : Channel("Ring Log") {}
    virtual ~RingLog() = default;

    // Raw protocol bytes ("ok", status frames) are not log lines; drop them.
    size_t write(uint8_t) override { return 1; }

    void print_msg(MsgLevel level, const char* msg) override;

    // Append the buffered lines, oldest first, to out.
    static void dump(std::string& out);
};

extern RingLog ringLog;
