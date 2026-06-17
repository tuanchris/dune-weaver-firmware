// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/SandStatus.h"

#include <string>

using namespace SandStatus;

// crude substring helper for readable assertions
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST(SandStatusEscape, PlainAndSpecials) {
    std::string o;
    append_escaped(o, "hello");
    EXPECT_EQ("\"hello\"", o);

    o.clear();
    append_escaped(o, "a\"b\\c");
    EXPECT_EQ("\"a\\\"b\\\\c\"", o);

    o.clear();
    append_escaped(o, "line\ntab\t");
    EXPECT_EQ("\"line\\ntab\\t\"", o);
}

TEST(SandStatusEscape, FilenamesWithSpacesAndParens) {
    // Real community pattern names; spaces/parens are legal JSON content
    std::string o;
    append_escaped(o, "/patterns/03 pnuttrellis (E) (N N).thr");
    EXPECT_EQ("\"/patterns/03 pnuttrellis (E) (N N).thr\"", o);
}

TEST(SandStatusEncode, IdleDefaults) {
    Data d;
    d.state = "Idle";
    std::string j = encode(d);
    EXPECT_TRUE(has(j, "\"state\":\"Idle\""));
    EXPECT_TRUE(has(j, "\"running\":false"));
    EXPECT_TRUE(has(j, "\"file\":\"\""));
    EXPECT_TRUE(has(j, "\"progress\":-1.0"));
    EXPECT_TRUE(has(j, "\"feed_override\":100"));  // default = 100%
    EXPECT_TRUE(has(j, "\"playlist\":{"));
    EXPECT_TRUE(has(j, "\"active\":false"));
    // No LED block unless has_led
    EXPECT_FALSE(has(j, "\"led\":"));
    // Well-formed object
    EXPECT_EQ('{', j.front());
    EXPECT_EQ('}', j.back());
}

TEST(SandStatusEncode, RunningWithProgressAndPlaylist) {
    Data d;
    d.state             = "Run";
    d.theta             = 1.2345f;
    d.rho               = 0.5f;
    d.feed              = 120.0f;
    d.feed_override     = 110;
    d.running           = true;
    d.file              = "/sd/star.thr";
    d.progress          = 42.5f;
    d.playlist_active   = true;
    d.playlist_index    = 2;
    d.playlist_total    = 10;
    d.playlist_name     = "evening";
    d.playlist_clearing = false;
    d.quiet             = false;

    std::string j = encode(d);
    EXPECT_TRUE(has(j, "\"state\":\"Run\""));
    EXPECT_TRUE(has(j, "\"theta\":1.2345"));
    EXPECT_TRUE(has(j, "\"rho\":0.5000"));
    EXPECT_TRUE(has(j, "\"feed\":120"));
    EXPECT_TRUE(has(j, "\"feed_override\":110"));
    EXPECT_TRUE(has(j, "\"running\":true"));
    EXPECT_TRUE(has(j, "\"file\":\"/sd/star.thr\""));
    EXPECT_TRUE(has(j, "\"progress\":42.5"));
    EXPECT_TRUE(has(j, "\"index\":2"));
    EXPECT_TRUE(has(j, "\"total\":10"));
    EXPECT_TRUE(has(j, "\"name\":\"evening\""));
}

TEST(SandStatusEncode, LedBlockWhenPresent) {
    Data d;
    d.state          = "Idle";
    d.has_led        = true;
    d.led_effect     = "rainbow";
    d.led_brightness = 40;
    std::string j    = encode(d);
    EXPECT_TRUE(has(j, "\"led\":{\"effect\":\"rainbow\",\"brightness\":40}"));
}

TEST(SandStatusEncode, FeedIsIntegerFormatted) {
    Data d;
    d.state = "Idle";
    d.feed  = 100.0f;
    EXPECT_TRUE(has(encode(d), "\"feed\":100"));
    EXPECT_FALSE(has(encode(d), "\"feed\":100.0"));
}

TEST(SandStatusArray, EmptyAndPopulated) {
    EXPECT_EQ("[]", encode_array({}));
    EXPECT_EQ("[\"/a.thr\"]", encode_array({ "/a.thr" }));
    EXPECT_EQ("[\"/a.thr\",\"/b.thr\"]", encode_array({ "/a.thr", "/b.thr" }));
}

TEST(SandStatusArray, EscapesEntries) {
    EXPECT_EQ("[\"a\\\"b.thr\"]", encode_array({ "a\"b.thr" }));
}

TEST(SandStatusObject, EmptyAndPopulated) {
    EXPECT_EQ("{}", encode_object({}));
    EXPECT_EQ("{\"THR/Feed\":\"100\"}", encode_object({ { "THR/Feed", "100" } }));
    EXPECT_EQ("{\"a\":\"1\",\"b\":\"2\"}", encode_object({ { "a", "1" }, { "b", "2" } }));
}

TEST(SandStatusObject, EscapesKeysAndValues) {
    EXPECT_EQ("{\"k\\\"\":\"v\\\\\"}", encode_object({ { "k\"", "v\\" } }));
}

TEST(ParseSdPercent, RealReports) {
    EXPECT_FLOAT_EQ(42.50f, parse_sd_percent("SD:42.50,/sd/star.thr"));
    EXPECT_FLOAT_EQ(0.0f, parse_sd_percent("SD:0.00,/sd/x.thr"));
    EXPECT_FLOAT_EQ(100.0f, parse_sd_percent("SD:100.00,/sd/x.thr"));
}

TEST(ParseSdPercent, NonProgressStrings) {
    EXPECT_FLOAT_EQ(-1.0f, parse_sd_percent(""));
    EXPECT_FLOAT_EQ(-1.0f, parse_sd_percent("SD: star.thr: Sent"));
    EXPECT_FLOAT_EQ(-1.0f, parse_sd_percent("garbage"));
}
