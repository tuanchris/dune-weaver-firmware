// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/Kinematics/ThrTranslator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using Kinematics::positive_mod_2pi;
using Kinematics::ThrLine;
using Kinematics::ThrTranslator;

static const float kTwoPi = 2.0f * float(M_PI);

// Translate `in` and return the result; the output line lands in `out`.
// outlen must be <= 256.
static ThrLine tr(ThrTranslator& t, const char* in, std::string& out, size_t outlen = 256) {
    char buf[256];
    strncpy(buf, in, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ThrLine status       = t.translate(buf, buf, outlen);
    out                  = buf;
    return status;
}

// Parse "G90G1X<theta>Y<rho>..." back into floats
static bool parseMove(const std::string& line, float& x, float& y) {
    return sscanf(line.c_str(), "G90G1X%fY%f", &x, &y) == 2;
}

TEST(PositiveMod2Pi, Basics) {
    EXPECT_FLOAT_EQ(0.0f, positive_mod_2pi(0.0f));
    EXPECT_NEAR(1.0f, positive_mod_2pi(1.0f), 1e-6f);
    EXPECT_NEAR(kTwoPi - 0.5f, positive_mod_2pi(-0.5f), 1e-5f);
    EXPECT_NEAR(1.234f, positive_mod_2pi(79.0f * kTwoPi + 1.234f), 1e-2f);
    float m = positive_mod_2pi(-1e-3f);
    EXPECT_GE(m, 0.0f);
    EXPECT_LT(m, kTwoPi);
}

TEST(ThrTranslator, SkipsBlankAndCommentLines) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Skip, tr(t, "", out));
    EXPECT_EQ(ThrLine::Skip, tr(t, "   ", out));
    EXPECT_EQ(ThrLine::Skip, tr(t, "\t\t", out));
    EXPECT_EQ(ThrLine::Skip, tr(t, "# a comment", out));
    EXPECT_EQ(ThrLine::Skip, tr(t, "  # indented comment", out));
}

TEST(ThrTranslator, RejectsInvalidLines) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Invalid, tr(t, "abc", out));
    EXPECT_EQ(ThrLine::Invalid, tr(t, "1.0", out));
    EXPECT_EQ(ThrLine::Invalid, tr(t, "1.0 xyz", out));
}

TEST(ThrTranslator, SimpleMoveWithFeedOnFirstLineOnly) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "0.5 0.3", out));
    EXPECT_EQ("G90G1X0.50000Y0.30000F100.0", out);
    EXPECT_EQ(ThrLine::Move, tr(t, "1 0.4", out));
    EXPECT_EQ("G90G1X1.00000Y0.40000", out);
}

TEST(ThrTranslator, NoFeedWhenDefaultFeedIsZero) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "0.5 0.3", out));
    EXPECT_EQ("G90G1X0.50000Y0.30000", out);
}

TEST(ThrTranslator, PreambleZeroPairsAreRhoOnlyMoves) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    // The first emitted move carries the feed, even when it is a
    // preamble rho-only move
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));
    EXPECT_EQ("G90G1Y0.00000F100.0", out);
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));
    EXPECT_EQ("G90G1Y0.00000", out);
    EXPECT_FALSE(t.offset_locked());
}

TEST(ThrTranslator, LeadingRevolutionsAreRemoved) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));

    // 79 whole revolutions plus 1 radian, like community patterns that
    // start at a large accumulated theta
    float theta = 79.0f * kTwoPi + 1.0f;
    char  line[64];
    snprintf(line, sizeof(line), "%.4f 0.7", theta);
    EXPECT_EQ(ThrLine::Move, tr(t, line, out));

    EXPECT_TRUE(t.offset_locked());
    EXPECT_NEAR(79.0f * kTwoPi, t.theta_offset(), 0.05f);

    float x, y;
    ASSERT_TRUE(parseMove(out, x, y));
    EXPECT_NEAR(1.0f, x, 2e-3f);
    EXPECT_GE(x, 0.0f);
    EXPECT_LT(x, kTwoPi);
    EXPECT_FLOAT_EQ(0.7f, y);
}

TEST(ThrTranslator, OffsetLocksOnFirstRealCoordinateOnly) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    // First real coordinate: theta=10 -> offset = 10 - mod(10) = 2pi
    EXPECT_EQ(ThrLine::Move, tr(t, "10 0.5", out));
    EXPECT_NEAR(kTwoPi, t.theta_offset(), 1e-4f);

    // Later thetas use the SAME offset; they are not re-normalized
    EXPECT_EQ(ThrLine::Move, tr(t, "20 0.5", out));
    float x, y;
    ASSERT_TRUE(parseMove(out, x, y));
    EXPECT_NEAR(20.0f - kTwoPi, x, 1e-3f);
}

TEST(ThrTranslator, NegativeThetaNormalizesIntoRange) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "-0.5 0.2", out));
    float x, y;
    ASSERT_TRUE(parseMove(out, x, y));
    EXPECT_NEAR(kTwoPi - 0.5f, x, 1e-3f);
}

TEST(ThrTranslator, RhoIsClamped) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "1 1.5", out));
    float x, y;
    ASSERT_TRUE(parseMove(out, x, y));
    EXPECT_FLOAT_EQ(1.0f, y);
    EXPECT_EQ(ThrLine::Move, tr(t, "2 -0.3", out));
    ASSERT_TRUE(parseMove(out, x, y));
    EXPECT_FLOAT_EQ(0.0f, y);
}

TEST(ThrTranslator, ZeroPairAfterPreambleIsARealMove) {
    ThrTranslator t;
    t.start(0.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "1 0.5", out));
    // Preamble is over; "0 0" is now a real coordinate, not rho-only
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));
    EXPECT_EQ("G90G1X0.00000Y0.00000", out);
}

TEST(ThrTranslator, OversizeKeepsFeedForNextMove) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    // Too small an output buffer: the line is dropped...
    EXPECT_EQ(ThrLine::Oversize, tr(t, "0.5 0.3", out, 10));
    // ...and the next successful move still carries the first-move feed
    EXPECT_EQ(ThrLine::Move, tr(t, "0.5 0.3", out));
    EXPECT_EQ("G90G1X0.50000Y0.30000F100.0", out);
}

TEST(ThrTranslator, TranslatesInPlace) {
    // The firmware passes the same buffer as input and output
    ThrTranslator t;
    t.start(0.0f);
    char buf[256];
    strcpy(buf, "0.5 0.3");
    EXPECT_EQ(ThrLine::Move, t.translate(buf, buf, sizeof(buf)));
    EXPECT_STREQ("G90G1X0.50000Y0.30000", buf);
}

TEST(ThrTranslator, StartResetsAllJobState) {
    ThrTranslator t;
    t.start(100.0f);
    std::string out;
    EXPECT_EQ(ThrLine::Move, tr(t, "10 0.5", out));
    EXPECT_TRUE(t.offset_locked());

    t.start(50.0f);
    EXPECT_FALSE(t.offset_locked());
    // Preamble handling and feed injection start over
    EXPECT_EQ(ThrLine::Move, tr(t, "0 0", out));
    EXPECT_EQ("G90G1Y0.00000F50.0", out);
}
