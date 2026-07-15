// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "CaptiveDns.h"

#include "src/DnsQuery.h"
#include "src/Logging.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <cstring>

namespace WebUI {
    namespace CaptiveDns {

        static int     s_sock  = -1;
        static uint8_t s_ip[4] = { 0, 0, 0, 0 };

        // Drain at most this many queries per poll() so a DNS flood cannot spin
        // the poller task; the rest wait for the next poll.
        static constexpr int MAX_PER_POLL = 8;

        void stop() {
            if (s_sock >= 0) {
                lwip_close(s_sock);
                s_sock = -1;
            }
        }

        void start(uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3) {
            stop();
            s_ip[0] = ip0;
            s_ip[1] = ip1;
            s_ip[2] = ip2;
            s_ip[3] = ip3;

            int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                log_error("Captive DNS: socket() failed");
                return;
            }

            // Non-blocking so poll() never stalls the poller task when idle.
            int nb = 1;
            lwip_ioctl(sock, FIONBIO, &nb);

            sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(53);
            addr.sin_addr.s_addr = 0;  // INADDR_ANY
            if (lwip_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                log_error("Captive DNS: bind :53 failed");
                lwip_close(sock);
                return;
            }

            s_sock = sock;
            log_info("Captive DNS started (lwIP)");
        }

        void poll(bool fallback) {
            if (s_sock < 0) {
                return;
            }
            uint8_t     buf[512];  // DNS-over-UDP without EDNS0 fits in 512
            sockaddr_in from;
            socklen_t   fromlen;

            for (int i = 0; i < MAX_PER_POLL; ++i) {
                fromlen = sizeof(from);
                int n   = lwip_recvfrom(s_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
                if (n <= 0) {
                    break;  // EWOULDBLOCK (nothing pending) or error -> done
                }
                DnsQuery::Question q = DnsQuery::parse(buf, static_cast<size_t>(n));
                if (!q.valid) {
                    continue;  // malformed / not a query -> ignore
                }
                DnsQuery::Action action = DnsQuery::decide(q, fallback);
                std::vector<uint8_t> resp =
                    DnsQuery::buildResponse(buf, static_cast<size_t>(n), q, action, s_ip[0], s_ip[1], s_ip[2], s_ip[3]);
                if (!resp.empty()) {
                    lwip_sendto(s_sock, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&from), fromlen);
                }
            }
        }
    }
}
