// Copyright 2022 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "TelnetClient.h"
#include "TelnetServer.h"

#include <WiFi.h>

namespace WebUI {
    TelnetClient::TelnetClient(WiFiClient* wifiClient) : Channel("telnet"), _wifiClient(wifiClient) {}

    void TelnetClient::handle() {}

    void TelnetClient::closeOnDisconnect() {
        // The check-and-set MUST be atomic across tasks: this runs in both the
        // poller (read path) and the output task (write path), and the old
        // non-atomic `if (_state != -1) { _state = -1; push; }` let both pass
        // the check and push the SAME pointer twice -> double free in
        // TelnetServer::poll().  Holding _disconnected_mtx across the whole
        // thing also makes the push mutually exclusive with the pop there.
        std::lock_guard<std::mutex> lock(TelnetServer::_disconnected_mtx);
        if (_state != -1 && !_wifiClient->connected()) {
            _state = -1;
            TelnetServer::_disconnected.push(this);
        }
    }

    void TelnetClient::flushRx() {
        Channel::flushRx();
    }

    size_t TelnetClient::write(uint8_t data) {
        return write(&data, 1);
    }

    size_t TelnetClient::write(const uint8_t* buffer, size_t length) {
        // Replace \n with \r\n
        size_t  rem      = length;
        uint8_t lastchar = '\0';
        size_t  j        = 0;
        while (rem) {
            const int bufsize = 128;
            uint8_t   modbuf[bufsize];
            // bufsize-1 in case the last character is \n
            size_t k = 0;
            while (rem && k < (bufsize - 1)) {
                uint8_t c = buffer[j++];
                if (c == '\n' && lastchar != '\r') {
                    modbuf[k++] = '\r';
                }
                lastchar    = c;
                modbuf[k++] = c;
                --rem;
            }
            if (k) {
                auto nWritten = _wifiClient->write(modbuf, k);
                if (nWritten == 0) {
                    closeOnDisconnect();
                    break;  // stop writing to a client we've just queued for reaping
                }
            }
        }
        return length;
    }

    int TelnetClient::peek(void) {
        return _wifiClient->peek();
    }

    int TelnetClient::available() {
        return _wifiClient->available();
    }

    int TelnetClient::rx_buffer_available() {
        return WIFI_CLIENT_READ_BUFFER_SIZE - available();
    }

    int TelnetClient::read(void) {
        if (_state == -1) {
            return -1;
        }
        // Loop so that consumed telnet negotiation bytes don't count as "no
        // data": keep reading until a real data byte appears or the socket is
        // drained.  The _iac state persists across calls, so a negotiation
        // sequence split across TCP segments is still consumed correctly.
        while (true) {
            int ret = _wifiClient->read();
            if (ret < 0) {
                // calling _wifiClient->connected() is expensive when the client is
                // connected because it calls recv() to double check, so we check
                // infrequently, only after quite a few reads have returned no data
                if (++_state >= DISCONNECT_CHECK_COUNTS) {
                    _state = 0;
                    closeOnDisconnect();  // sets _state to -1 if disconnected
                }
                return -1;
            }
            // Got a byte; reset the disconnect counter.
            _state = 0;
            switch (_iac) {
                case 0:
                    if (ret == 0xFF) {  // IAC: start of a telnet command
                        _iac = 1;
                        continue;
                    }
                    return ret;
                case 1:  // command byte after IAC
                    if (ret == 0xFF) {  // escaped literal 0xFF in the data stream
                        _iac = 0;
                        return 0xFF;
                    }
                    if (ret >= 0xFB && ret <= 0xFE) {  // WILL/WONT/DO/DONT: one option byte follows
                        _iac = 2;
                        continue;
                    }
                    if (ret == 0xFA) {  // SB: subnegotiation runs until IAC SE
                        _iac = 3;
                        continue;
                    }
                    _iac = 0;  // NOP/DM/etc: a bare 2-byte command, nothing more to eat
                    continue;
                case 2:  // the option byte after WILL/WONT/DO/DONT: discard it
                    _iac = 0;
                    continue;
                case 3:  // inside subnegotiation: scan for the closing IAC
                    if (ret == 0xFF) {
                        _iac = 4;
                    }
                    continue;
                default:  // 4: subnegotiation, just saw IAC
                    _iac = (ret == 0xF0) ? 0 : 3;  // SE ends it; otherwise stay in SB
                    continue;
            }
        }
    }

    TelnetClient::~TelnetClient() {
        delete _wifiClient;
    }
}
