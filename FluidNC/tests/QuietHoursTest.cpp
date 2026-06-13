// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/QuietHours.h"

using namespace QuietHours;

// tm_wday convention: 0=Sunday .. 6=Saturday
static constexpr int SUN = 0, MON = 1, TUE = 2, WED = 3, FRI = 5, SAT = 6;

static int t(int h, int m) {
    return h * 60 + m;
}

TEST(QuietHoursParse, SingleDailySlot) {
    auto slots = parse("09:00-20:00@daily");
    ASSERT_EQ(1u, slots.size());
    EXPECT_EQ(t(9, 0), slots[0].start_min);
    EXPECT_EQ(t(20, 0), slots[0].end_min);
    EXPECT_EQ(0x7F, slots[0].days);
}

TEST(QuietHoursParse, MultipleSlotsWithSpaces) {
    auto slots = parse(" 09:00-20:00@weekdays , 22:30-06:00@weekends ");
    ASSERT_EQ(2u, slots.size());
    EXPECT_EQ(0x3E, slots[0].days);  // mon..fri
    EXPECT_EQ(0x41, slots[1].days);  // sun, sat
    EXPECT_EQ(t(22, 30), slots[1].start_min);
}

TEST(QuietHoursParse, CustomDayList) {
    auto slots = parse("13:00-14:00@mon+tue+fri");
    ASSERT_EQ(1u, slots.size());
    EXPECT_EQ((1 << MON) | (1 << TUE) | (1 << FRI), slots[0].days);
}

TEST(QuietHoursParse, EmptySpecIsNoSlots) {
    EXPECT_TRUE(parse("").empty());
    EXPECT_TRUE(parse(" , ").empty());
}

TEST(QuietHoursParse, ErrorsReportAndReturnEmpty) {
    std::string err;
    EXPECT_TRUE(parse("25:00-26:00@daily", &err).empty());
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(parse("09:00-20:00@noday", &err).empty());
    EXPECT_TRUE(parse("09:00/20:00@daily", &err).empty());
    EXPECT_TRUE(parse("garbage", &err).empty());
    // One bad slot poisons the whole spec (predictable all-or-nothing)
    EXPECT_TRUE(parse("09:00-20:00@daily,bad", &err).empty());
}

TEST(QuietHoursMatch, SimpleWindowInclusiveBounds) {
    auto slots = parse("09:00-20:00@daily");
    EXPECT_FALSE(match(slots, MON, t(8, 59)));
    EXPECT_TRUE(match(slots, MON, t(9, 0)));
    EXPECT_TRUE(match(slots, MON, t(14, 30)));
    EXPECT_TRUE(match(slots, MON, t(20, 0)));
    EXPECT_FALSE(match(slots, MON, t(20, 1)));
}

TEST(QuietHoursMatch, MidnightWrap) {
    auto slots = parse("22:00-06:00@daily");
    EXPECT_TRUE(match(slots, MON, t(23, 0)));
    EXPECT_TRUE(match(slots, MON, t(0, 0)));
    EXPECT_TRUE(match(slots, MON, t(6, 0)));
    EXPECT_FALSE(match(slots, MON, t(6, 1)));
    EXPECT_FALSE(match(slots, MON, t(12, 0)));
    EXPECT_TRUE(match(slots, MON, t(22, 0)));
}

TEST(QuietHoursMatch, DayFilterAppliesToCurrentDay) {
    // dune-weaver semantics: a Monday 22:00-06:00 slot matches Monday
    // 23:00 and the *current-day* tail — i.e. only if the new day (Tue)
    // is in the set it would match at 02:00.  With only Monday in the
    // set, Tuesday 02:00 does NOT match.
    auto slots = parse("22:00-06:00@mon");
    EXPECT_TRUE(match(slots, MON, t(23, 0)));
    EXPECT_FALSE(match(slots, TUE, t(2, 0)));
    // Monday 02:00 matches (the tail "belonging" to Sunday night)
    EXPECT_TRUE(match(slots, MON, t(2, 0)));
}

TEST(QuietHoursMatch, WeekdayAndWeekendSets) {
    auto slots = parse("09:00-17:00@weekdays");
    EXPECT_TRUE(match(slots, WED, t(10, 0)));
    EXPECT_FALSE(match(slots, SAT, t(10, 0)));
    EXPECT_FALSE(match(slots, SUN, t(10, 0)));

    slots = parse("09:00-17:00@weekends");
    EXPECT_FALSE(match(slots, WED, t(10, 0)));
    EXPECT_TRUE(match(slots, SAT, t(10, 0)));
    EXPECT_TRUE(match(slots, SUN, t(10, 0)));
}

TEST(QuietHoursMatch, MultipleSlotsAnyMatchWins) {
    auto slots = parse("09:00-10:00@daily,15:00-16:00@daily");
    EXPECT_TRUE(match(slots, MON, t(9, 30)));
    EXPECT_TRUE(match(slots, MON, t(15, 30)));
    EXPECT_FALSE(match(slots, MON, t(12, 0)));
}

TEST(QuietHoursMatch, EmptySlotsNeverMatch) {
    EXPECT_FALSE(match({}, MON, t(12, 0)));
}
