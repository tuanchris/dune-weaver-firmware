#include <gtest/gtest.h>
#include "src/HoldClock.h"

TEST(HoldClock, NeverHeldIsZero) {
    HoldClock c;
    c.set_held(false, 100);
    EXPECT_EQ(c.held(1000), 0);
}

TEST(HoldClock, CountsAnOpenHoldUpToNow) {
    HoldClock c;
    c.set_held(true, 100);
    EXPECT_EQ(c.held(100), 0);
    EXPECT_EQ(c.held(350), 250);
}

TEST(HoldClock, AddsFinishedHolds) {
    HoldClock c;
    c.set_held(true, 100);
    c.set_held(false, 300);  // 200
    c.set_held(true, 1000);
    c.set_held(false, 1050);  // 50
    EXPECT_EQ(c.held(5000), 250);
}

TEST(HoldClock, RepeatedStatesDoNotRestartTheHold) {
    // set_state(Hold) is called again while held (a second feedhold request):
    // the hold must keep its original start.
    HoldClock c;
    c.set_held(true, 100);
    c.set_held(true, 400);
    c.set_held(false, 500);
    c.set_held(false, 900);
    EXPECT_EQ(c.held(1000), 400);
}

TEST(HoldClock, JobSeesOnlyItsOwnHolds) {
    // What InputFile does: note held() at the start, subtract what accrued since.
    HoldClock c;
    c.set_held(true, 0);
    c.set_held(false, 1000);  // an earlier job's pause
    const int64_t start = 2000, atStart = c.held(start);
    c.set_held(true, 5000);
    c.set_held(false, 8000);  // this job's pause: 3000
    const int64_t now = 10000;
    EXPECT_EQ((now - start) - (c.held(now) - atStart), 5000);
    // ...and while paused, the job's run time stands still.
    c.set_held(true, 10000);
    EXPECT_EQ((12000 - start) - (c.held(12000) - atStart), 5000);
}
