// Copyright (c) 2022 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "src/Channel.h"

#include <WiFi.h>

namespace WebUI {
    class TelnetClient : public Channel {
        WiFiClient* _wifiClient;

        // The default value of the rx buffer in WiFiClient.cpp is 1436 which is
        // related to the network frame size minus TCP/IP header sizes.
        // The WiFiClient API has no way to override or query it.
        // We use a smaller value for safety.  There is little advantage
        // to sending too many GCode lines at once, especially since the
        // common serial communication case is typically limited to 128 bytes.
        static const int WIFI_CLIENT_READ_BUFFER_SIZE = 1200;

        static const int DISCONNECT_CHECK_COUNTS = 1000;

        int _state = 0;

        // Telnet IAC (0xFF) negotiation state, consumed in read() so option
        // bytes never reach the Grbl realtime parser.  A scanner's "IAC WILL
        // TERMINAL-TYPE" is literally FF FB 18, and 0x18 == Ctrl-X == reset:
        // ordinary telnet negotiation used to soft-reset the controller.
        // 0 = normal, 1 = after IAC, 2 = after WILL/WONT/DO/DONT (option
        // follows), 3 = in subnegotiation, 4 = in subnegotiation after IAC.
        int _iac = 0;

    public:
        TelnetClient(WiFiClient* wifiClient);

        int    rx_buffer_available() override;
        size_t write(uint8_t data) override;
        size_t write(const uint8_t* buffer, size_t size) override;
        int    read(void) override;
        int    peek(void) override;
        int    available() override;
        void   flush() override {}
        void   flushRx() override;

        void closeOnDisconnect();

        void handle() override;

        ~TelnetClient();
    };
}
