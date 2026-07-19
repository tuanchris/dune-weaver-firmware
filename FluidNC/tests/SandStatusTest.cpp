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
    EXPECT_TRUE(has(j, "\"progress\":-1,"));  // unknown sentinel, clean "-1"
    EXPECT_TRUE(has(j, "\"pause_remaining\":-1"));  // not pausing
    EXPECT_TRUE(has(j, "\"pause_total\":-1"));
    EXPECT_TRUE(has(j, "\"feed_override\":100"));  // default = 100%
    EXPECT_TRUE(has(j, "\"playlist\":{"));
    EXPECT_TRUE(has(j, "\"active\":false"));
    // No LED block unless has_led
    EXPECT_FALSE(has(j, "\"led\":"));
    // Health fields omitted at their defaults
    EXPECT_FALSE(has(j, "\"sd_ok\":"));
    EXPECT_FALSE(has(j, "\"last_reset\":"));
    EXPECT_FALSE(has(j, "\"uptime\":"));
    // Well-formed object
    EXPECT_EQ('{', j.front());
    EXPECT_EQ('}', j.back());
}

TEST(SandStatusEncode, HealthFieldsWhenSet) {
    Data d;
    d.state        = "Idle";
    d.has_sd       = true;
    d.sd_ok        = false;
    d.last_reset   = "panic";
    d.uptime       = 4242;
    d.heap         = 145000;
    d.heap_min     = 98000;
    d.heap_largest = 60000;
    std::string j = encode(d);
    EXPECT_TRUE(has(j, "\"sd_ok\":false"));
    EXPECT_TRUE(has(j, "\"last_reset\":\"panic\""));
    EXPECT_TRUE(has(j, "\"uptime\":4242"));
    EXPECT_TRUE(has(j, "\"heap\":145000"));
    EXPECT_TRUE(has(j, "\"heap_min\":98000"));
    EXPECT_TRUE(has(j, "\"heap_largest\":60000"));
    EXPECT_FALSE(has(j, "\"fw\":"));  // omitted while unset

    d.fw = "v0.1.3 (test)";
    EXPECT_TRUE(has(encode(d), "\"fw\":\"v0.1.3 (test)\""));
    EXPECT_EQ('}', j.back());

    d.sd_ok = true;
    EXPECT_TRUE(has(encode(d), "\"sd_ok\":true"));
}

TEST(SandStatusEncode, IdentityFieldsWhenSet) {
    Data d;
    d.state       = "Idle";
    std::string j = encode(d);
    EXPECT_FALSE(has(j, "\"mac\":"));       // omitted while unset
    EXPECT_FALSE(has(j, "\"hostname\":"));  // omitted while unset

    d.mac      = "a0:b1:c2:d3:e4:f5";
    d.hostname = "DWMP";
    j          = encode(d);
    EXPECT_TRUE(has(j, "\"mac\":\"a0:b1:c2:d3:e4:f5\""));
    EXPECT_TRUE(has(j, "\"hostname\":\"DWMP\""));

    // Empty strings are treated like unset (e.g. hostname not yet applied).
    d.mac      = "";
    d.hostname = "";
    j          = encode(d);
    EXPECT_FALSE(has(j, "\"mac\":"));
    EXPECT_FALSE(has(j, "\"hostname\":"));
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
    d.progress          = 0.425f;  // 0..1 fraction
    d.playlist_active   = true;
    d.playlist_index    = 2;
    d.playlist_total    = 10;
    d.playlist_pause_remaining = 42;
    d.playlist_pause_total     = 60;
    d.playlist_name     = "evening";
    d.playlist_next     = "/patterns/owl.thr";
    d.playlist_last     = "/patterns/star.thr";
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
    EXPECT_TRUE(has(j, "\"progress\":0.425"));
    EXPECT_TRUE(has(j, "\"pause_remaining\":42"));
    EXPECT_TRUE(has(j, "\"pause_total\":60"));
    EXPECT_TRUE(has(j, "\"index\":2"));
    EXPECT_TRUE(has(j, "\"total\":10"));
    EXPECT_TRUE(has(j, "\"name\":\"evening\""));
    // Shuffle-aware "up next" (resolved by the firmware; the app must not
    // guess it from the unshuffled file order).
    EXPECT_TRUE(has(j, "\"next\":\"/patterns/owl.thr\""));
    // Just-finished pattern = what's drawn on the table now (for a preview
    // during the between-patterns pause).
    EXPECT_TRUE(has(j, "\"last\":\"/patterns/star.thr\""));
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

TEST(ExecutedPercent, ZeroSizeIsZero) {
    EXPECT_FLOAT_EQ(0.0f, executed_percent(0, 0, 0, 0));
    EXPECT_FLOAT_EQ(0.0f, executed_percent(100, 0, 5, 0));
}

TEST(ExecutedPercent, NoQueuedEqualsRawReadPosition) {
    // queued_blocks == 0 -> no look-ahead to subtract -> bytes-read / size
    EXPECT_NEAR(50.0f, executed_percent(500, 1000, 100, 0), 0.001f);
    EXPECT_NEAR(100.0f, executed_percent(1000, 1000, 100, 0), 0.001f);
}

TEST(ExecutedPercent, SubtractsLookaheadAtStart) {
    // 16 lines read (~10 bytes each), planner holds 15 -> almost all of the read
    // bytes are still queued, so executed progress is ~0, not the naive 8%.
    // avg=160/16=10, inflight=15*10=150, executed=10 -> 10/2000 = 0.5%
    EXPECT_NEAR(0.5f, executed_percent(160, 2000, 16, 15), 0.01f);
    // naive bytes-read/size would have been 8%:
    EXPECT_NEAR(8.0f, 160.0f * 100.0f / 2000.0f, 0.01f);
}

TEST(ExecutedPercent, MidRunRemovesConstantLead) {
    // avg=1000/100=10, inflight=15*10=150, executed=850 -> 42.5%  (naive 50%)
    EXPECT_NEAR(42.5f, executed_percent(1000, 2000, 100, 15), 0.01f);
}

TEST(ExecutedPercent, FloorsAtZeroAndClampsAt100) {
    // inflight exceeds the read position -> floored at 0
    EXPECT_FLOAT_EQ(0.0f, executed_percent(100, 2000, 5, 50));
    // position beyond size (shouldn't happen) -> clamped to 100
    EXPECT_FLOAT_EQ(100.0f, executed_percent(2000, 1000, 100, 0));
}
