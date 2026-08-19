#include <gtest/gtest.h>

#include "src/LedHook.h"

// $LED/RunEffect / $LED/IdleEffect ids used in these tests (see Leds.h).
static const int OFF     = 0;
static const int STATIC  = 1;
static const int BREATHE = 3;
static const int NONE    = -1;  // "none" as an EnumSetting reads it back

TEST(LedHook, NoOverrideBeforeAnyStateReport) {
    LedHook h;
    EXPECT_EQ(h.cat(), LedHook::CatNone);
    EXPECT_EQ(h.effect(OFF, OFF), -1);  // boot: never override, even with hooks set
}

TEST(LedHook, IdleHookAppliesWhileIdle) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(NONE, BREATHE), BREATHE);
    EXPECT_EQ(h.effect(BREATHE, NONE), -1);  // the run hook must not leak into idle
}

TEST(LedHook, RunHookAppliesWhileRunning) {
    LedHook h;
    h.reportCat(LedHook::CatRun);
    EXPECT_EQ(h.effect(BREATHE, NONE), BREATHE);
    EXPECT_EQ(h.effect(NONE, BREATHE), -1);
}

TEST(LedHook, NoneMeansNoOverride) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(NONE, NONE), -1);
}

// The bug this class exists for: $LED/IdleEffect=off used to swallow every
// "turn the LEDs on" at an idle table, so nothing but brightness 0 appeared
// to work.
TEST(LedHook, ManualChoiceBeatsTheHook) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(NONE, OFF), OFF);  // strip held dark by the hook
    h.manualChoice();                     // user taps the app's power button
    EXPECT_EQ(h.effect(NONE, OFF), -1);   // ... and the manual effect renders
}

TEST(LedHook, SameCategoryReportedAgainKeepsTheManualChoice) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    h.manualChoice();
    h.reportCat(LedHook::CatIdle);  // 1 Hz status reports must not undo it
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(NONE, OFF), -1);
}

TEST(LedHook, ChangingStateCategoryReArmsTheHook) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    h.manualChoice();
    EXPECT_EQ(h.effect(OFF, OFF), -1);
    h.reportCat(LedHook::CatRun);  // a pattern starts: the run hook takes over
    EXPECT_EQ(h.effect(STATIC, OFF), STATIC);
    h.reportCat(LedHook::CatIdle);  // and back to idle: the idle hook is armed again
    EXPECT_EQ(h.effect(STATIC, OFF), OFF);
}

TEST(LedHook, ManualChoiceInOneCategoryDoesNotSuppressTheOther) {
    LedHook h;
    h.reportCat(LedHook::CatRun);
    h.manualChoice();  // tweaked the LEDs mid-pattern
    EXPECT_EQ(h.effect(OFF, BREATHE), -1);
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(OFF, BREATHE), BREATHE);  // the idle hook still means what it says
}

TEST(LedHook, ClearingAHookTakesEffectImmediately) {
    LedHook h;
    h.reportCat(LedHook::CatIdle);
    EXPECT_EQ(h.effect(NONE, OFF), OFF);
    // The settings are read on every call, so $LED/IdleEffect=none needs no
    // state change to be honored -- the app's Clear button works at once.
    EXPECT_EQ(h.effect(NONE, NONE), -1);
}
