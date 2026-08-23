#include <gtest/gtest.h>

#include "src/LedWhite.h"

TEST(LedWhite, ParsesRgbOrders) {
    LedWhite::Order o;
    ASSERT_TRUE(LedWhite::parseOrder("GRB", o));
    EXPECT_EQ(o.bpp, 3);
    EXPECT_FALSE(o.rgbw());
    EXPECT_EQ(o.gi, 0);
    EXPECT_EQ(o.ri, 1);
    EXPECT_EQ(o.bi, 2);

    ASSERT_TRUE(LedWhite::parseOrder("rgb", o));  // case-insensitive
    EXPECT_EQ(o.ri, 0);
    EXPECT_EQ(o.gi, 1);
    EXPECT_EQ(o.bi, 2);
    EXPECT_EQ(o.wi, -1);
}

TEST(LedWhite, ParsesRgbwOrders) {
    LedWhite::Order o;
    ASSERT_TRUE(LedWhite::parseOrder("GRBW", o));  // SK6812 RGBW
    EXPECT_EQ(o.bpp, 4);
    EXPECT_TRUE(o.rgbw());
    EXPECT_EQ(o.gi, 0);
    EXPECT_EQ(o.ri, 1);
    EXPECT_EQ(o.bi, 2);
    EXPECT_EQ(o.wi, 3);

    ASSERT_TRUE(LedWhite::parseOrder("WRGB", o));  // W first is legal too
    EXPECT_EQ(o.wi, 0);
    EXPECT_EQ(o.ri, 1);
}

TEST(LedWhite, RejectsBadOrders) {
    LedWhite::Order o;
    o.bpp = 99;  // must be untouched on failure
    EXPECT_FALSE(LedWhite::parseOrder("RGG", o));    // repeated letter
    EXPECT_FALSE(LedWhite::parseOrder("RG", o));     // missing B
    EXPECT_FALSE(LedWhite::parseOrder("RGBX", o));   // unknown letter
    EXPECT_FALSE(LedWhite::parseOrder("RGBWW", o));  // two W
    EXPECT_FALSE(LedWhite::parseOrder("RGBWR", o));  // too long
    EXPECT_FALSE(LedWhite::parseOrder("", o));
    EXPECT_FALSE(LedWhite::parseOrder(nullptr, o));
    EXPECT_EQ(o.bpp, 99);
}

// The four modes, checked against WLED's autoWhiteCalc on an orange-ish color.
TEST(LedWhite, AutoWhiteNone) {
    uint8_t r = 200, g = 100, b = 50, w = 77;
    LedWhite::autoWhite(LedWhite::None, r, g, b, w);
    EXPECT_EQ(r, 200);
    EXPECT_EQ(g, 100);
    EXPECT_EQ(b, 50);
    EXPECT_EQ(w, 0);
}

TEST(LedWhite, AutoWhiteBrighter) {
    uint8_t r = 200, g = 100, b = 50, w = 0;
    LedWhite::autoWhite(LedWhite::Brighter, r, g, b, w);
    EXPECT_EQ(r, 200);  // RGB untouched
    EXPECT_EQ(g, 100);
    EXPECT_EQ(b, 50);
    EXPECT_EQ(w, 50);  // min channel
}

TEST(LedWhite, AutoWhiteAccurate) {
    uint8_t r = 200, g = 100, b = 50, w = 0;
    LedWhite::autoWhite(LedWhite::Accurate, r, g, b, w);
    EXPECT_EQ(w, 50);
    EXPECT_EQ(r, 150);  // min subtracted from every channel
    EXPECT_EQ(g, 50);
    EXPECT_EQ(b, 0);
}

TEST(LedWhite, AutoWhiteMax) {
    uint8_t r = 200, g = 100, b = 50, w = 0;
    LedWhite::autoWhite(LedWhite::Max, r, g, b, w);
    EXPECT_EQ(r, 200);
    EXPECT_EQ(g, 100);
    EXPECT_EQ(b, 50);
    EXPECT_EQ(w, 200);  // max channel
}

TEST(LedWhite, AccuratePureWhiteMovesEntirelyToW) {
    uint8_t r = 255, g = 255, b = 255, w = 0;
    LedWhite::autoWhite(LedWhite::Accurate, r, g, b, w);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(w, 255);
}

TEST(LedWhite, AccurateSaturatedColorLeavesWDark) {
    uint8_t r = 255, g = 0, b = 0, w = 0;
    LedWhite::autoWhite(LedWhite::Accurate, r, g, b, w);
    EXPECT_EQ(r, 255);
    EXPECT_EQ(w, 0);
}

TEST(LedWhite, PackRgbStripIgnoresWhiteMode) {
    LedWhite::Order o;
    ASSERT_TRUE(LedWhite::parseOrder("GRB", o));
    uint8_t p[3] = { 9, 9, 9 };
    LedWhite::pack(o, LedWhite::Accurate, p, 200, 100, 50, 255);
    EXPECT_EQ(p[0], 100);  // G
    EXPECT_EQ(p[1], 200);  // R
    EXPECT_EQ(p[2], 50);   // B -- no subtraction on a 3-byte strip
}

TEST(LedWhite, PackRgbwStripWritesWireOrder) {
    LedWhite::Order o;
    ASSERT_TRUE(LedWhite::parseOrder("GRBW", o));
    uint8_t p[4] = { 9, 9, 9, 9 };
    LedWhite::pack(o, LedWhite::Accurate, p, 200, 100, 50, 255);
    EXPECT_EQ(p[0], 50);   // G  (100-50)
    EXPECT_EQ(p[1], 150);  // R  (200-50)
    EXPECT_EQ(p[2], 0);    // B  (50-50)
    EXPECT_EQ(p[3], 50);   // W
}

TEST(LedWhite, PackScalesWByBrightnessToo) {
    LedWhite::Order o;
    ASSERT_TRUE(LedWhite::parseOrder("RGBW", o));
    uint8_t p[4] = { 0, 0, 0, 0 };
    LedWhite::pack(o, LedWhite::Brighter, p, 255, 255, 255, 127);  // scale 128/256
    EXPECT_EQ(p[0], 127);
    EXPECT_EQ(p[1], 127);
    EXPECT_EQ(p[2], 127);
    EXPECT_EQ(p[3], 127);

    LedWhite::pack(o, LedWhite::Brighter, p, 255, 255, 255, 0);  // brightness 0 = dark, W included
    EXPECT_EQ(p[0], 0);
    EXPECT_EQ(p[3], 0);
}
