// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Pure DNS-query parsing and captive-portal policy, separated from the lwIP
  socket I/O (WebUI/CaptiveDns) so it can be unit-tested in the native test
  environment.

  The sand table runs a captive DNS responder while in AP mode.  It replaces
  the arduino-esp32 DNSServer, whose single "resolve everything to one IP"
  wildcard funnelled the connected phone's *entire* background traffic at the
  table's :80 (every app's hostname -> softAPIP -> a TCP connection the
  single-client web server cannot drain -> heap/PCB exhaustion -> wedge).

  The policy here answers per-query instead:
    - Fallback AP (unconfigured / failed home-WiFi join): resolve everything
      to the table so the OS captive sheet pops for setup.  (old behavior)
    - Standalone AP ($WiFi/Mode=AP): resolve ONLY the OS connectivity-probe
      hosts to the table -- so the phone's probe succeeds, it treats the
      hotspot as "online" and stays attached -- and refuse everything else
      (NXDOMAIN) so background apps never open a socket to the table.
*/

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DnsQuery {

    constexpr uint16_t TYPE_A    = 1;
    constexpr uint16_t TYPE_AAAA = 28;

    // The first question parsed out of a DNS query packet.
    struct Question {
        bool        valid = false;  // false => malformed/unsupported; drop the packet
        uint16_t    id    = 0;      // transaction id, echoed into the response
        uint16_t    qtype = 0;      // TYPE_A, TYPE_AAAA, ...
        std::string name;           // lowercased dotted QNAME, no trailing dot
        size_t      qend  = 0;      // byte offset just past the question (QCLASS)
        uint8_t     rd    = 0;      // recursion-desired bit, echoed back
    };

    // What to do with one question.
    enum class Action : uint8_t {
        AnswerA,   // return an A record pointing at the table
        NoData,    // NOERROR, no answer (name exists, no record of this type)
        NxDomain,  // RCODE 3, name does not exist -> client backs off
    };

    // Parse the first question of a standard DNS query.  Returns valid=false
    // on anything truncated, malformed, compressed-in-question, not a query,
    // or without at least one question -- the caller then ignores the packet.
    Question parse(const uint8_t* buf, size_t len);

    // Is this hostname one of the OS connectivity-probe hosts?  Case-
    // insensitive exact FQDN match against the built-in table.
    bool isProbeHost(const std::string& name);

    // Captive-portal policy for one question given the AP role.
    Action decide(const Question& q, bool fallback);

    // Build the DNS response for a parsed question + chosen action.  ip0..ip3
    // are the table's softAP IPv4 octets in dotted order (a.b.c.d).  The
    // question section is echoed verbatim from the query; an AnswerA appends
    // a single A record using a compression pointer back to the QNAME.
    std::vector<uint8_t> buildResponse(const uint8_t* query,
                                       size_t         queryLen,
                                       const Question& q,
                                       Action          action,
                                       uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3);
}
