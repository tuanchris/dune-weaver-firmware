// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/PlaylistParse.h"

using namespace PlaylistParse;

TEST(ParsePlaylist, BasicLinesWithCommentsAndWhitespace) {
    std::string content = "# evening rotation\n"
                          "/patterns/star.thr\n"
                          "\n"
                          "  patterns/ibex.thr  \n"
                          "/patterns/spiral.thr  # the good one\n"
                          "\t\n";
    auto items = parse_playlist(content, 100);
    ASSERT_EQ(3u, items.size());
    EXPECT_EQ("/patterns/star.thr", items[0]);
    EXPECT_EQ("/patterns/ibex.thr", items[1]);  // leading slash added
    EXPECT_EQ("/patterns/spiral.thr", items[2]);
}

TEST(ParsePlaylist, HandlesCrlfAndMissingTrailingNewline) {
    std::string content = "/a.thr\r\n/b.thr";
    auto        items   = parse_playlist(content, 100);
    ASSERT_EQ(2u, items.size());
    EXPECT_EQ("/a.thr", items[0]);
    EXPECT_EQ("/b.thr", items[1]);
}

TEST(ParsePlaylist, EmptyAndCommentOnlyContent) {
    EXPECT_TRUE(parse_playlist("", 100).empty());
    EXPECT_TRUE(parse_playlist("# nothing\n  \n\t# more\n", 100).empty());
}

TEST(ParsePlaylist, RespectsMaxItems) {
    std::string content;
    for (int i = 0; i < 20; i++) {
        content += "/p" + std::to_string(i) + ".thr\n";
    }
    auto items = parse_playlist(content, 5);
    ASSERT_EQ(5u, items.size());
    EXPECT_EQ("/p0.thr", items[0]);
    EXPECT_EQ("/p4.thr", items[4]);
}

TEST(ParsePlaylist, PatternNamesWithSpaces) {
    auto items = parse_playlist("patterns/03 pnuttrellis (E) (N N).thr\n", 100);
    ASSERT_EQ(1u, items.size());
    EXPECT_EQ("/patterns/03 pnuttrellis (E) (N N).thr", items[0]);
}

TEST(FirstRho, SimplePattern) {
    EXPECT_FLOAT_EQ(0.25f, first_rho("1.5 0.25\n2.0 0.5\n"));
}

TEST(FirstRho, SkipsCommentsAndGarbage) {
    EXPECT_FLOAT_EQ(0.7f, first_rho("# header\nnot a coord\n3.0 0.7\n"));
}

TEST(FirstRho, PreambleZeroPairsUseThirdCoordinate) {
    // Two leading "0 0" pairs mean the third coordinate is the real start
    EXPECT_FLOAT_EQ(0.9f, first_rho("0 0\n0 0\n400.5 0.9\n"));
}

TEST(FirstRho, SingleZeroPairIsTheRealStart) {
    // Only one zero pair: it IS the first coordinate (rho 0)
    EXPECT_FLOAT_EQ(0.0f, first_rho("0 0\n5.0 0.8\n1.0 0.1\n"));
}

TEST(FirstRho, NoCoordinates) {
    EXPECT_FLOAT_EQ(-1.0f, first_rho(""));
    EXPECT_FLOAT_EQ(-1.0f, first_rho("# only comments\n"));
}

TEST(FirstRho, MissingTrailingNewline) {
    EXPECT_FLOAT_EQ(0.33f, first_rho("2.0 0.33"));
}

TEST(ChooseClear, FixedModes) {
    EXPECT_EQ(Clear::None, choose_clear(CLEAR_NONE, 0.5f, 0));
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_IN, 0.5f, 0));
    EXPECT_EQ(Clear::FromOut, choose_clear(CLEAR_OUT, 0.5f, 0));
    EXPECT_EQ(Clear::Sideway, choose_clear(CLEAR_SIDEWAY, 0.5f, 0));
    EXPECT_EQ(Clear::None, choose_clear(99, 0.5f, 0));  // unknown mode
}

TEST(ChooseClear, RandomUsesSuppliedEntropy) {
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_RANDOM, 0.5f, 0));
    EXPECT_EQ(Clear::FromOut, choose_clear(CLEAR_RANDOM, 0.5f, 1));
    EXPECT_EQ(Clear::Sideway, choose_clear(CLEAR_RANDOM, 0.5f, 2));
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_RANDOM, 0.5f, 3));
}

TEST(ChooseClear, AdaptiveMatchesDuneWeaverPolicy) {
    // Pattern starting near the center -> clear that ends at the center
    EXPECT_EQ(Clear::FromOut, choose_clear(CLEAR_ADAPTIVE, 0.0f, 0));
    EXPECT_EQ(Clear::FromOut, choose_clear(CLEAR_ADAPTIVE, 0.49f, 0));
    // Pattern starting near the rim -> clear that ends at the rim
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_ADAPTIVE, 0.5f, 0));
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_ADAPTIVE, 1.0f, 0));
}

TEST(ChooseClear, AdaptiveUnknownRhoFallsBackToRandom) {
    EXPECT_EQ(Clear::FromIn, choose_clear(CLEAR_ADAPTIVE, -1.0f, 0));
    EXPECT_EQ(Clear::FromOut, choose_clear(CLEAR_ADAPTIVE, -1.0f, 1));
    EXPECT_EQ(Clear::Sideway, choose_clear(CLEAR_ADAPTIVE, -1.0f, 2));
}

TEST(ParseClearMode, KnownNamesCaseInsensitive) {
    int m = -99;
    EXPECT_TRUE(parse_clear_mode("none", m));
    EXPECT_EQ(CLEAR_NONE, m);
    EXPECT_TRUE(parse_clear_mode("Adaptive", m));
    EXPECT_EQ(CLEAR_ADAPTIVE, m);
    EXPECT_TRUE(parse_clear_mode("IN", m));
    EXPECT_EQ(CLEAR_IN, m);
    EXPECT_TRUE(parse_clear_mode("out", m));
    EXPECT_EQ(CLEAR_OUT, m);
    EXPECT_TRUE(parse_clear_mode("sideway", m));
    EXPECT_EQ(CLEAR_SIDEWAY, m);
    EXPECT_TRUE(parse_clear_mode("random", m));
    EXPECT_EQ(CLEAR_RANDOM, m);
}

TEST(ParseClearMode, SideIsAliasForSideway) {
    int m = -99;
    EXPECT_TRUE(parse_clear_mode("side", m));
    EXPECT_EQ(CLEAR_SIDEWAY, m);
}

TEST(ParseClearMode, UnknownOrEmptyLeavesModeUntouched) {
    int m = 42;
    EXPECT_FALSE(parse_clear_mode("bogus", m));
    EXPECT_EQ(42, m);
    EXPECT_FALSE(parse_clear_mode("", m));
    EXPECT_EQ(42, m);
    EXPECT_FALSE(parse_clear_mode(nullptr, m));
    EXPECT_EQ(42, m);
}
