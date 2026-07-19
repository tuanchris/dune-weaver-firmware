// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "gtest/gtest.h"
#include "src/HttpRange.h"

using HttpRange::parse;
using HttpRange::Result;

static Result run(const char* value, size_t fileSize, size_t& start, size_t& len) {
    start = len = 0;
    return parse(value, fileSize, start, len);
}

TEST(HttpRange, AbsentOrForeignHeader) {
    size_t s, l;
    EXPECT_EQ(run(nullptr, 100, s, l), Result::None);
    EXPECT_EQ(run("", 100, s, l), Result::None);
    EXPECT_EQ(run("items=0-5", 100, s, l), Result::None);
    EXPECT_EQ(run("bytes", 100, s, l), Result::None);
}

TEST(HttpRange, ClosedRange) {
    size_t s, l;
    EXPECT_EQ(run("bytes=0-1023", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 0u);
    EXPECT_EQ(l, 1024u);

    EXPECT_EQ(run("bytes=100-100", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 100u);
    EXPECT_EQ(l, 1u);
}

TEST(HttpRange, ClosedRangeClampedToEof) {
    size_t s, l;
    EXPECT_EQ(run("bytes=4000-9999", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 4000u);
    EXPECT_EQ(l, 1000u);
}

TEST(HttpRange, OpenRange) {
    size_t s, l;
    EXPECT_EQ(run("bytes=4500-", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 4500u);
    EXPECT_EQ(l, 500u);
}

TEST(HttpRange, SuffixRange) {
    size_t s, l;
    EXPECT_EQ(run("bytes=-500", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 4500u);
    EXPECT_EQ(l, 500u);

    // Suffix longer than the file serves the whole file as a 206.
    EXPECT_EQ(run("bytes=-9999", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 0u);
    EXPECT_EQ(l, 5000u);
}

TEST(HttpRange, Unsatisfiable) {
    size_t s, l;
    EXPECT_EQ(run("bytes=5000-", 5000, s, l), Result::Unsatisfiable);
    EXPECT_EQ(run("bytes=5000-6000", 5000, s, l), Result::Unsatisfiable);
    EXPECT_EQ(run("bytes=-0", 5000, s, l), Result::Unsatisfiable);
    EXPECT_EQ(run("bytes=0-", 0, s, l), Result::Unsatisfiable);
    EXPECT_EQ(run("bytes=-5", 0, s, l), Result::Unsatisfiable);
}

TEST(HttpRange, MalformedServesWhole) {
    size_t s, l;
    EXPECT_EQ(run("bytes=5-2", 5000, s, l), Result::None);       // inverted
    EXPECT_EQ(run("bytes=a-b", 5000, s, l), Result::None);
    EXPECT_EQ(run("bytes=1-2-3", 5000, s, l), Result::None);
    EXPECT_EQ(run("bytes=1- 5", 5000, s, l), Result::None);      // stray space
    EXPECT_EQ(run("bytes=-", 5000, s, l), Result::None);
    EXPECT_EQ(run("bytes=", 5000, s, l), Result::None);
}

TEST(HttpRange, MultiRangeServesWhole) {
    size_t s, l;
    EXPECT_EQ(run("bytes=0-5,10-15", 5000, s, l), Result::None);
}

TEST(HttpRange, HugeNumbersSaturateSafely) {
    size_t s, l;
    // 30-digit start: must not wrap into a valid offset.
    EXPECT_EQ(run("bytes=999999999999999999999999999999-", 5000, s, l), Result::Unsatisfiable);
    EXPECT_EQ(run("bytes=0-999999999999999999999999999999", 5000, s, l), Result::Partial);
    EXPECT_EQ(s, 0u);
    EXPECT_EQ(l, 5000u);
}
