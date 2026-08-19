// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "src/Module.h"  // Module
#include <queue>
#include <mutex>

#include "src/Settings.h"

#include <WiFi.h>

class TelnetClient;

namespace WebUI {
    class TelnetServer : public Module {
        // OFF by default.  This is a headless HTTP-driven table; the raw Grbl
        // telnet port is pure attack surface (network scanners' option
        // negotiation injects realtime bytes -- 0x18 reset, 0x84 door -- and
        // the accept/disconnect paths were the top field-panic cluster).  Users
        // who want a serial-over-TCP console can still enable $Telnet/Enable=ON;
        // the code paths below are hardened for that case.
        static const int DEFAULT_TELNET_STATE      = 0;
        static const int DEFAULT_TELNETSERVER_PORT = 23;

        static const int MAX_TELNET_PORT = 65001;
        static const int MIN_TELNET_PORT = 1;

        // Hard cap on concurrent telnet clients (was only the listen backlog).
        // Each client costs a WiFiClient + a full Channel (~2-3 KB) plus an lwIP
        // socket, and unbounded accepts also starve the single-client HTTP
        // server of sockets.
        static const int MAX_TLNT_CLIENTS = 2;

        // Refuse a new telnet connection when the largest allocatable block is
        // below this: constructing a client allocates ~1.5 KB, and the accept
        // path had no heap guard at all (HTTP has one).  Sits above the web
        // server's 6 KB hard floor so HTTP wins the last of the heap.
        static const uint32_t ACCEPT_HEAP_FLOOR = 12000;

        static const int FLUSHTIMEOUT = 500;

    public:
        TelnetServer(const char* name) : Module(name) {}

        static uint16_t port() { return _port; }

        // Clients pending deletion, plus the lock that makes push (from the
        // poller task AND the output task) and pop+delete (poller) mutually
        // exclusive.  Without it the unguarded std::queue was a concurrent
        // deque push/pop, and the non-atomic close guard allowed a double push
        // -> double free.
        static std::queue<TelnetClient*> _disconnected;
        static std::mutex                _disconnected_mtx;
        static int                       _client_count;

        void init() override;
        void deinit() override;
        void poll() override;
        void status_report(Channel& out) override;

        ~TelnetServer();

    private:
        bool            _setupdone  = false;
        WiFiServer*     _wifiServer = nullptr;
        static uint16_t _port;
    };
}
