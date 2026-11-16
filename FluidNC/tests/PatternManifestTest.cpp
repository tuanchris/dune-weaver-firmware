// Copyright (c) 2026 - Dune Weaver
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/PatternManifest.h"

#include <string>

using namespace PatternManifest;

namespace {
    // Apply append() to a whole catalog the way the firmware does: from its last 64 bytes.
    std::string appended(const std::string& file, const std::string& key) {
        size_t      tailAt = file.size() > 64 ? file.size() - 64 : 0;
        size_t      at     = 0;
        std::string text;
        if (!append(file.substr(tailAt), tailAt, quoted(key), at, text)) {
            return "<refused>";
        }
        std::string out = file;
        out.replace(at, std::min(text.size(), out.size() - at), text);
        if (at + text.size() > out.size()) out += text.substr(out.size() - at);
        return out;
    }

    bool found(const std::string& file, const std::string& key, size_t cap) {
        size_t pos  = 0;
        auto   read = [&](char* dst, size_t max) -> size_t {
            size_t n = std::min({ max, size_t(7), file.size() - pos });  // small reads: windows straddle
            memcpy(dst, file.data() + pos, n);
            pos += n;
            return n;
        };
        std::string buf(cap, '\0');
        return contains(read, quoted(key), &buf[0], cap);
    }
}

TEST(PatternManifest, KeyFor) {
    EXPECT_EQ(keyFor("/sd/patterns/owl.thr"), "owl.thr");
    EXPECT_EQ(keyFor("/sd/patterns/custom_patterns/a b.THR"), "custom_patterns/a b.THR");
    EXPECT_EQ(keyFor("/sd/patterns/wipe_in.gcode"), "wipe_in.gcode");
    EXPECT_EQ(keyFor("/sd/patterns/x.nc"), "x.nc");
    EXPECT_EQ(keyFor("/sd/patterns/index.json"), "");
    EXPECT_EQ(keyFor("/sd/patterns/previews/shard-0.zip"), "");
    EXPECT_EQ(keyFor("/sd/patterns/previews/x.thr"), "");
    EXPECT_EQ(keyFor("/sd/playlists/fav.txt"), "");
    EXPECT_EQ(keyFor("/sd/owl.thr"), "");
    EXPECT_EQ(keyFor("/sd/patterns/"), "");
}

TEST(PatternManifest, Quoted) {
    EXPECT_EQ(quoted("owl.thr"), "\"owl.thr\"");
    EXPECT_EQ(quoted("a\"b\\c.thr"), "\"a\\\"b\\\\c.thr\"");
    EXPECT_EQ(quoted(std::string("t\tab.thr")), "\"t\\u0009ab.thr\"");
    EXPECT_EQ(quoted("caf\xc3\xa9.thr"), "\"caf\xc3\xa9.thr\"");  // UTF-8 as is
}

TEST(PatternManifest, AppendsInPlace) {
    EXPECT_EQ(appended("[\"a.thr\",\"b.thr\"]", "c.thr"), "[\"a.thr\",\"b.thr\",\"c.thr\"]");
    EXPECT_EQ(appended("[]", "c.thr"), "[\"c.thr\"]");
    EXPECT_EQ(appended("[ ]\n", "c.thr"), "[ \"c.thr\"]");
    EXPECT_EQ(appended("[\"a.thr\"]\n", "c.thr"), "[\"a.thr\",\"c.thr\"]");
    EXPECT_EQ(appended("[\n  \"a.thr\"\n]\n", "c.thr"), "[\n  \"a.thr\"\n,\"c.thr\"]");
    // A long catalog: only its tail is read.
    std::string big = "[";
    for (int i = 0; i < 500; ++i) big += std::string(i ? "," : "") + "\"pattern-" + std::to_string(i) + ".thr\"";
    big += "]";
    EXPECT_EQ(appended(big, "new.thr"), big.substr(0, big.size() - 1) + ",\"new.thr\"]");
}

TEST(PatternManifest, RefusesWhatIsntACatalog) {
    EXPECT_EQ(appended("", "c.thr"), "<refused>");
    EXPECT_EQ(appended("{\"a\":1}", "c.thr"), "<refused>");
    EXPECT_EQ(appended("[1,2]", "c.thr"), "<refused>");
    EXPECT_EQ(appended("[\"a.thr\"", "c.thr"), "<refused>");  // truncated
    EXPECT_EQ(appended("[\"a.thr\"]" + std::string(40, ' '), "c.thr"), "<refused>");  // more trailing space than the write covers
}

TEST(PatternManifest, Contains) {
    std::string file = "[\"a.thr\",\"owl.thr\",\"custom/owl.thr\",\"owl.thr.bak\"]";
    for (size_t cap : { size_t(16), size_t(64), size_t(512) }) {
        EXPECT_TRUE(found(file, "owl.thr", cap)) << cap;
        EXPECT_TRUE(found(file, "custom/owl.thr", cap)) << cap;
        EXPECT_FALSE(found(file, "wl.thr", cap)) << cap;  // quoted: no partial-name match
        EXPECT_FALSE(found(file, "owl", cap)) << cap;
        EXPECT_FALSE(found(file, "b.thr", cap)) << cap;
    }
    EXPECT_FALSE(found("", "a.thr", 64));
}
