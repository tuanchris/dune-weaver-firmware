// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "DnsQuery.h"

#include <cctype>
#include <cstring>

namespace DnsQuery {

    // OS connectivity-probe hosts.  In standalone AP these resolve to the
    // table so each OS's "am I online?" check succeeds and the phone stays
    // attached; every other name is refused.  Extend as OSes change probes --
    // keep it to genuine probe hosts, not general CDNs, or the storm returns.
    static const char* const kProbeHosts[] = {
        "captive.apple.com",              // iOS / macOS
        "www.apple.com",                  // iOS / macOS (hotspot-detect secondary)
        "connectivitycheck.gstatic.com",  // Android
        "connectivitycheck.android.com",  // Android (legacy)
        "clients3.google.com",            // Android (legacy generate_204)
        "www.msftconnecttest.com",        // Windows 10+
        "dns.msftncsi.com",               // Windows (NCSI)
        "www.msftncsi.com",               // Windows (NCSI legacy)
        "detectportal.firefox.com",       // Firefox
    };

    bool isProbeHost(const std::string& name) {
        for (const char* h : kProbeHosts) {
            if (name == h) {
                return true;
            }
        }
        return false;
    }

    static uint16_t rd16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    Question parse(const uint8_t* buf, size_t len) {
        Question q;
        if (!buf || len < 12) {
            return q;  // no room for a header
        }
        const uint8_t flagsHi = buf[2];
        // QR must be 0 (a query), opcode must be 0 (standard query).
        if ((flagsHi & 0x80) != 0 || (flagsHi & 0x78) != 0) {
            return q;
        }
        const uint16_t qdcount = rd16(buf + 4);
        if (qdcount < 1) {
            return q;
        }

        // Decode the QNAME labels starting at offset 12.
        std::string name;
        size_t      off = 12;
        while (true) {
            if (off >= len) {
                return q;  // ran off the end before the root label
            }
            const uint8_t label = buf[off];
            if ((label & 0xC0) != 0) {
                // A compression pointer (or reserved bits) in the question is
                // not something a normal client sends; refuse rather than
                // chase it.
                return q;
            }
            if (label == 0) {
                ++off;  // consume the root label
                break;
            }
            if (off + 1 + label > len) {
                return q;  // label overruns the packet
            }
            if (!name.empty()) {
                name.push_back('.');
            }
            for (size_t i = 0; i < label; ++i) {
                name.push_back(static_cast<char>(std::tolower(buf[off + 1 + i])));
            }
            off += 1 + label;
            if (name.size() > 255) {
                return q;  // absurdly long name
            }
        }

        // QTYPE + QCLASS follow the name.
        if (off + 4 > len) {
            return q;
        }
        q.qtype = rd16(buf + off);
        // (QCLASS at off+2 is ignored; we only serve IN, and refusing on
        // class mismatch would just drop the packet -- same effect.)
        q.qend  = off + 4;

        q.valid = true;
        q.id    = rd16(buf);
        q.rd    = static_cast<uint8_t>(buf[2] & 0x01);
        q.name  = std::move(name);
        return q;
    }

    Action decide(const Question& q, bool fallback) {
        const bool wantsIPv4 = (q.qtype == TYPE_A);
        if (fallback) {
            // Setup portal: every name points at the table so the captive
            // sheet pops.  Only IPv4 exists (softAP is v4); v6 gets NODATA so
            // the client falls back to the A record.
            return wantsIPv4 ? Action::AnswerA : Action::NoData;
        }
        // Standalone: only the probe hosts resolve.
        if (isProbeHost(q.name)) {
            return wantsIPv4 ? Action::AnswerA : Action::NoData;
        }
        return Action::NxDomain;
    }

    std::vector<uint8_t> buildResponse(const uint8_t* query,
                                       size_t         queryLen,
                                       const Question& q,
                                       Action          action,
                                       uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3) {
        std::vector<uint8_t> out;
        if (!q.valid || q.qend > queryLen) {
            return out;
        }
        const bool answer = (action == Action::AnswerA);

        // Header: 12 bytes.
        out.reserve(q.qend + (answer ? 16 : 0));
        out.push_back(static_cast<uint8_t>(q.id >> 8));
        out.push_back(static_cast<uint8_t>(q.id & 0xFF));
        // flags hi: QR=1, opcode=0, AA=1, TC=0, RD echoed.
        out.push_back(static_cast<uint8_t>(0x84 | (q.rd & 0x01)));
        // flags lo: RA=0, Z=0, RCODE (3 for NXDOMAIN, else 0).
        out.push_back(static_cast<uint8_t>(action == Action::NxDomain ? 0x03 : 0x00));
        // QDCOUNT = 1
        out.push_back(0x00);
        out.push_back(0x01);
        // ANCOUNT = answer ? 1 : 0
        out.push_back(0x00);
        out.push_back(answer ? 0x01 : 0x00);
        // NSCOUNT = 0, ARCOUNT = 0
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);

        // Echo the question verbatim (QNAME + QTYPE + QCLASS), i.e. bytes
        // [12, qend) of the original query.
        out.insert(out.end(), query + 12, query + q.qend);

        if (answer) {
            // Answer: name via compression pointer to the QNAME at offset 12.
            out.push_back(0xC0);
            out.push_back(0x0C);
            out.push_back(0x00);  // TYPE A
            out.push_back(0x01);
            out.push_back(0x00);  // CLASS IN
            out.push_back(0x01);
            out.push_back(0x00);  // TTL = 30s (short: policy can change live)
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x1E);
            out.push_back(0x00);  // RDLENGTH = 4
            out.push_back(0x04);
            out.push_back(ip0);
            out.push_back(ip1);
            out.push_back(ip2);
            out.push_back(ip3);
        }
        return out;
    }
}
