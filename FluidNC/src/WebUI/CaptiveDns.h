// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Captive-portal DNS responder, native lwIP.  Replaces the arduino-esp32
  DNSServer (which could only wildcard every name to one IP -- the funnel
  that let a phone's background traffic storm the table's web server in AP
  mode).  The per-query policy lives in the std-only, unit-tested DnsQuery;
  this module is just the UDP socket + pump.

  Lifecycle: start() once when the AP web server comes up; poll() from
  Web_Server::poll() while in AP mode, passing the live fallback state so a
  standalone<->fallback flip takes effect without a restart.
*/

#include <cstdint>

namespace WebUI {
    namespace CaptiveDns {
        // Open a non-blocking UDP socket bound to :53.  ip0..ip3 are the
        // softAP IPv4 octets answered for resolved names.  Idempotent.
        void start(uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3);

        // Answer any pending queries (bounded per call).  fallback selects the
        // policy: true = resolve everything (setup portal), false = resolve
        // only OS probe hosts (standalone).  No-op if not started.
        void poll(bool fallback);

        // Close the socket.  Safe to call when not started.
        void stop();
    }
}
