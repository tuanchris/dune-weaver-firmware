#include <gtest/gtest.h>

#include "src/LedPalettes.h"

// Ids are persisted in NVS: the original eight must never move.
TEST(LedPalettes, OriginalIdsAreStable) {
    const char* original[] = { "default", "ocean", "lava", "forest", "party", "cloud", "heat", "sunset" };
    for (int i = 0; i < 8; i++) {
        EXPECT_EQ(LedPalettes::find(original[i]), i) << original[i];
    }
    EXPECT_EQ(LedPalettes::kTables[0][0], 0x191970u);  // ocean, which pacifica draws by default
}

TEST(LedPalettes, RainbowIsTheOldNameOfDefault) {
    EXPECT_EQ(LedPalettes::find("rainbow"), 0);  // older apps still send it
    EXPECT_EQ(LedPalettes::find("spectrum"), 8);  // WLED's Rainbow table
}

TEST(LedPalettes, FindsEveryNameAndRejectsUnknown) {
    for (int i = 0; i < LedPalettes::kCount; i++) {
        EXPECT_EQ(LedPalettes::find(LedPalettes::kNames[i]), i) << LedPalettes::kNames[i];
    }
    EXPECT_EQ(LedPalettes::find("candy2"), LedPalettes::kCount - 1);
    EXPECT_EQ(LedPalettes::find("Ocean"), -1);  // exact, like EnumSetting
    EXPECT_EQ(LedPalettes::find(""), -1);
    EXPECT_EQ(LedPalettes::find("nope"), -1);
    EXPECT_LE(LedPalettes::kCount, 127);  // stored as int8_t
}
