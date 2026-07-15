// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/DnsQuery.h"

#include <string>
#include <vector>

using namespace DnsQuery;

// Encode a minimal single-question DNS query packet.
static std::vector<uint8_t> makeQuery(uint16_t id, const std::string& name, uint16_t qtype, uint8_t rd = 1) {
    std::vector<uint8_t> p = {
        static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xFF),
        static_cast<uint8_t>(rd & 0x01), 0x00,  // flags: QR=0, opcode=0, RD
        0x00, 0x01,                             // QDCOUNT = 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // AN/NS/AR = 0
    };
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        p.push_back(static_cast<uint8_t>(dot - start));
        for (size_t i = start; i < dot; ++i) {
            p.push_back(static_cast<uint8_t>(name[i]));
        }
        if (dot == name.size()) {
            break;
        }
        start = dot + 1;
    }
    p.push_back(0x00);                                  // root label
    p.push_back(static_cast<uint8_t>(qtype >> 8));
    p.push_back(static_cast<uint8_t>(qtype & 0xFF));
    p.push_back(0x00);
    p.push_back(0x01);                                  // QCLASS IN
    return p;
}

TEST(DnsParse, BasicAQuery) {
    auto pkt = makeQuery(0xBEEF, "captive.apple.com", TYPE_A);
    auto q   = parse(pkt.data(), pkt.size());
    ASSERT_TRUE(q.valid);
    EXPECT_EQ(0xBEEF, q.id);
    EXPECT_EQ(TYPE_A, q.qtype);
    EXPECT_EQ("captive.apple.com", q.name);
    EXPECT_EQ(pkt.size(), q.qend);
    EXPECT_EQ(1, q.rd);
}

TEST(DnsParse, LowercasesName) {
    auto pkt = makeQuery(1, "Captive.Apple.COM", TYPE_A);
    auto q   = parse(pkt.data(), pkt.size());
    ASSERT_TRUE(q.valid);
    EXPECT_EQ("captive.apple.com", q.name);
}

TEST(DnsParse, RejectsTooShort) {
    uint8_t buf[5] = { 0, 0, 0, 0, 0 };
    EXPECT_FALSE(parse(buf, sizeof(buf)).valid);
    EXPECT_FALSE(parse(nullptr, 0).valid);
}

TEST(DnsParse, RejectsResponsePacket) {
    auto pkt = makeQuery(1, "example.com", TYPE_A);
    pkt[2] |= 0x80;  // set QR -> it's a response, not a query
    EXPECT_FALSE(parse(pkt.data(), pkt.size()).valid);
}

TEST(DnsParse, RejectsTruncatedName) {
    auto pkt = makeQuery(1, "example.com", TYPE_A);
    pkt.resize(15);  // chop mid-QNAME
    EXPECT_FALSE(parse(pkt.data(), pkt.size()).valid);
}

TEST(DnsParse, RejectsCompressionPointerInQuestion) {
    auto pkt = makeQuery(1, "example.com", TYPE_A);
    pkt[12]  = 0xC0;  // first label byte becomes a compression pointer
    pkt[13]  = 0x0C;
    EXPECT_FALSE(parse(pkt.data(), pkt.size()).valid);
}

TEST(DnsPolicy, ProbeHostRecognition) {
    EXPECT_TRUE(isProbeHost("captive.apple.com"));
    EXPECT_TRUE(isProbeHost("connectivitycheck.gstatic.com"));
    EXPECT_TRUE(isProbeHost("www.msftconnecttest.com"));
    EXPECT_FALSE(isProbeHost("push.apple.com"));
    EXPECT_FALSE(isProbeHost("example.com"));
    EXPECT_FALSE(isProbeHost("analytics.tiktok.com"));
}

static Question q_for(const std::string& name, uint16_t qtype) {
    auto pkt = makeQuery(1, name, qtype);
    return parse(pkt.data(), pkt.size());
}

TEST(DnsPolicy, FallbackResolvesEverything) {
    EXPECT_EQ(Action::AnswerA, decide(q_for("anything.example", TYPE_A), /*fallback=*/true));
    EXPECT_EQ(Action::AnswerA, decide(q_for("push.apple.com", TYPE_A), true));
    // No IPv6 on softAP -> NODATA so the client falls back to the A record.
    EXPECT_EQ(Action::NoData, decide(q_for("captive.apple.com", TYPE_AAAA), true));
}

TEST(DnsPolicy, StandaloneOnlyResolvesProbes) {
    EXPECT_EQ(Action::AnswerA, decide(q_for("captive.apple.com", TYPE_A), /*fallback=*/false));
    EXPECT_EQ(Action::NoData, decide(q_for("captive.apple.com", TYPE_AAAA), false));
    // The storm sources -> refused so they never open a socket to the table.
    EXPECT_EQ(Action::NxDomain, decide(q_for("push.apple.com", TYPE_A), false));
    EXPECT_EQ(Action::NxDomain, decide(q_for("graph.facebook.com", TYPE_A), false));
    EXPECT_EQ(Action::NxDomain, decide(q_for("example.com", TYPE_AAAA), false));
}

TEST(DnsBuild, AnswerARecord) {
    auto pkt = makeQuery(0x1234, "captive.apple.com", TYPE_A);
    auto q   = parse(pkt.data(), pkt.size());
    auto r   = buildResponse(pkt.data(), pkt.size(), q, Action::AnswerA, 192, 168, 0, 1);

    ASSERT_EQ(pkt.size() + 16u, r.size());  // question echoed + 16-byte A answer
    EXPECT_EQ(0x12, r[0]);
    EXPECT_EQ(0x34, r[1]);
    EXPECT_EQ(0x85, r[2]);  // QR=1, AA=1, RD=1
    EXPECT_EQ(0x00, r[3]);  // RCODE 0
    EXPECT_EQ(0x00, r[4]);
    EXPECT_EQ(0x01, r[5]);  // QDCOUNT 1
    EXPECT_EQ(0x00, r[6]);
    EXPECT_EQ(0x01, r[7]);  // ANCOUNT 1

    // Question echoed verbatim.
    for (size_t i = 12; i < pkt.size(); ++i) {
        EXPECT_EQ(pkt[i], r[i]) << "question byte " << i;
    }

    // Answer record right after the echoed question.
    const uint8_t* a = r.data() + pkt.size();
    EXPECT_EQ(0xC0, a[0]);
    EXPECT_EQ(0x0C, a[1]);  // name pointer -> offset 12
    EXPECT_EQ(0x00, a[2]);
    EXPECT_EQ(0x01, a[3]);  // TYPE A
    EXPECT_EQ(0x00, a[4]);
    EXPECT_EQ(0x01, a[5]);  // CLASS IN
    EXPECT_EQ(0x00, a[10]);
    EXPECT_EQ(0x04, a[11]);  // RDLENGTH 4
    EXPECT_EQ(192, a[12]);
    EXPECT_EQ(168, a[13]);
    EXPECT_EQ(0, a[14]);
    EXPECT_EQ(1, a[15]);
}

TEST(DnsBuild, NxDomainHasNoAnswer) {
    auto pkt = makeQuery(0x2222, "example.com", TYPE_A);
    auto q   = parse(pkt.data(), pkt.size());
    auto r   = buildResponse(pkt.data(), pkt.size(), q, Action::NxDomain, 192, 168, 0, 1);

    ASSERT_EQ(pkt.size(), r.size());  // header + question only
    EXPECT_EQ(0x03, r[3] & 0x0F);     // RCODE 3 (NXDOMAIN)
    EXPECT_EQ(0x00, r[6]);
    EXPECT_EQ(0x00, r[7]);            // ANCOUNT 0
}

TEST(DnsBuild, NoDataHasNoAnswerButNoError) {
    auto pkt = makeQuery(0x3333, "captive.apple.com", TYPE_AAAA);
    auto q   = parse(pkt.data(), pkt.size());
    auto r   = buildResponse(pkt.data(), pkt.size(), q, Action::NoData, 192, 168, 0, 1);

    ASSERT_EQ(pkt.size(), r.size());
    EXPECT_EQ(0x00, r[3] & 0x0F);  // RCODE 0 (NOERROR)
    EXPECT_EQ(0x00, r[7]);         // ANCOUNT 0
}
