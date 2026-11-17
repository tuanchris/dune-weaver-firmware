// Copyright (c) 2026 - Dune Weaver
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/MultipartScan.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using MultipartScan::copyPart;
using MultipartScan::find;

namespace {
    const std::string kPat = "\r\n--XyZboundary42";

    struct Copied {
        std::string data;  // what flush() received, in order
        std::string rest;  // what copyPart left after the boundary
        long        result;
        size_t      flushes;
        size_t      smallest;  // smallest flush (SD write) but the last
    };

    // Feed `body` (already-read prefix `pre` first) to copyPart, the client
    // handing over at most `chunk` bytes per read, into a buffer of `cap`.
    Copied copy(const std::string& body, size_t chunk, size_t cap, const std::string& pre = "", size_t align = 1) {
        std::vector<unsigned char> buf(cap);
        memcpy(buf.data(), pre.data(), pre.size());
        size_t pos = 0;
        Copied    r { "", "", 0, 0, SIZE_MAX };
        std::vector<size_t> sizes;
        auto read = [&](unsigned char* dst, size_t max) -> long {
            size_t n = std::min({ max, chunk, body.size() - pos });
            memcpy(dst, body.data() + pos, n);
            pos += n;
            return static_cast<long>(n);
        };
        auto flush = [&](size_t n) {
            r.data.append(reinterpret_cast<const char*>(buf.data()), n);
            sizes.push_back(n);
        };
        r.result  = copyPart(buf.data(), cap, pre.size(), kPat.c_str(), kPat.size(), read, flush, align);
        r.flushes = sizes.size();
        for (size_t i = 0; i + 1 < sizes.size(); ++i) r.smallest = std::min(r.smallest, sizes[i]);
        if (r.result > 0) r.rest.assign(reinterpret_cast<const char*>(buf.data()), r.result);
        r.rest += body.substr(pos);  // and what the client still holds
        return r;
    }

    std::string tricky(size_t n, unsigned seed) {
        // Random bytes, salted with boundary look-alikes and bare \r\n--.
        std::mt19937 g(seed);
        std::string  s;
        while (s.size() < n) {
            switch (g() % 6) {
                case 0: s += "\r\n--"; break;
                case 1: s += kPat.substr(0, 1 + g() % (kPat.size() - 1)); break;
                case 2: s += '\r'; break;
                default: s += static_cast<char>(g() & 0xff); break;
            }
        }
        s.resize(n);
        return s;
    }
}

TEST(MultipartScan, Find) {
    const std::string s = "ab\r\n--XyZboundary42cd";
    EXPECT_EQ(find(reinterpret_cast<const unsigned char*>(s.data()), s.size(), kPat.c_str(), kPat.size()), 2);
    EXPECT_EQ(find(reinterpret_cast<const unsigned char*>(s.data()), 10, kPat.c_str(), kPat.size()), -1);
    const std::string miss = "\r\n--XyZboundary4\r\n--XyZboundary42";
    EXPECT_EQ(find(reinterpret_cast<const unsigned char*>(miss.data()), miss.size(), kPat.c_str(), kPat.size()), 16);  // "\r\n--XyZboundary4" is 16 bytes
}

TEST(MultipartScan, EveryChunkingKeepsTheDataExact) {
    for (unsigned seed : { 1u, 2u, 3u }) {
        const std::string data = tricky(3000, seed);
        const std::string body = data + kPat + "--\r\n";
        for (size_t cap : { kPat.size(), size_t(64), size_t(1000), size_t(8192) }) {
            for (size_t chunk = 1; chunk <= 300; chunk += (chunk < 40 ? 1 : 37)) {
                Copied r = copy(body, chunk, cap);
                ASSERT_EQ(r.data, data) << "seed " << seed << " cap " << cap << " chunk " << chunk;
                ASSERT_EQ(r.rest, "--\r\n") << "cap " << cap << " chunk " << chunk;
            }
        }
    }
}

TEST(MultipartScan, WritesAreWholeBuffersButTheLast) {
    const std::string data = tricky(20000, 7);
    Copied               r    = copy(data + kPat + "--\r\n", 1460, 8192);
    EXPECT_EQ(r.data, data);
    EXPECT_EQ(r.flushes, 3u);  // 8192 - 16, again, then the rest
    EXPECT_EQ(r.smallest, 8192 - (kPat.size() - 1));
}

TEST(MultipartScan, AlignedWritesAreExactMultiples) {
    // The firmware's buffer is 8192 + 128 and flushes align to the 4096-byte stdio buffer.
    const std::string data = tricky(50000, 11);
    std::vector<size_t> sizes;
    for (size_t chunk : { size_t(1), size_t(536), size_t(1460), size_t(5000) }) {
        Copied r = copy(data + kPat + "--\r\n", chunk, 8192 + 128, "", 4096);
        EXPECT_EQ(r.data, data) << "chunk " << chunk;
        EXPECT_EQ(r.smallest, 8192u) << "chunk " << chunk;  // every flush but the last is 8 KB
        EXPECT_EQ(r.flushes, 50000 / 8192 + 1) << "chunk " << chunk;
        EXPECT_EQ(r.rest, "--\r\n");
    }
    // A tiny buffer can't round: flushes stay correct.
    Copied t = copy(data + kPat + "--\r\n", 100, 64, "", 4096);
    EXPECT_EQ(t.data, data);
}

TEST(MultipartScan, PartsAfterTheFileAreLeftForTheParser) {
    const std::string next = "\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nvalue" + kPat + "--\r\n";
    Copied               r    = copy("hello" + kPat + next, 4096, 8192);
    EXPECT_EQ(r.data, "hello");
    EXPECT_EQ(r.rest, next);
}

TEST(MultipartScan, BytesReadBeforeTheFileComeFirst) {
    // A previous part's read ran into this one: its first bytes are in the buffer already.
    const std::string data = tricky(500, 9);
    const std::string body = data + kPat + "--\r\n";
    for (size_t pre : { size_t(0), size_t(1), size_t(16), size_t(200) }) {
        Copied r = copy(body.substr(pre), 50, 256, body.substr(0, pre));
        EXPECT_EQ(r.data, data) << "pre " << pre;
        EXPECT_EQ(r.rest, "--\r\n");
    }
}

TEST(MultipartScan, EmptyFileAndBoundaryInThePrefix) {
    Copied r = copy(kPat + "--\r\n", 3, 64);
    EXPECT_EQ(r.data, "");
    EXPECT_EQ(r.flushes, 0u);
    EXPECT_EQ(r.rest, "--\r\n");
    Copied p = copy("", 10, 64, "ab" + kPat + "--\r\n");
    EXPECT_EQ(p.data, "ab");
    EXPECT_EQ(p.rest, "--\r\n");
}

TEST(MultipartScan, ClientGoneBeforeTheBoundary) {
    Copied r = copy("truncated upload with no boundary", 7, 64);
    EXPECT_EQ(r.result, -1);
}
