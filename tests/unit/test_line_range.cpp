#include <gtest/gtest.h>
#include "line_range.hpp"

using ghost::note::LineRangeSet;

// ─── Parse Tests ───────────────────────────────────────────────────────────

TEST(LineRangeSetParse, Empty) {
    auto set = LineRangeSet::parse("");
    EXPECT_TRUE(set.empty());
    EXPECT_EQ(set.lineCount(), 0);
}

TEST(LineRangeSetParse, SingleNumber) {
    auto set = LineRangeSet::parse("42");
    EXPECT_FALSE(set.empty());
    EXPECT_EQ(set.lineCount(), 1);
    EXPECT_TRUE(set.contains(42));
    EXPECT_FALSE(set.contains(41));
    EXPECT_FALSE(set.contains(43));
}

TEST(LineRangeSetParse, SimpleRange) {
    auto set = LineRangeSet::parse("5-12");
    EXPECT_EQ(set.lineCount(), 8);  // 5,6,7,8,9,10,11,12
    EXPECT_TRUE(set.contains(5));
    EXPECT_TRUE(set.contains(12));
    EXPECT_FALSE(set.contains(4));
    EXPECT_FALSE(set.contains(13));
}

TEST(LineRangeSetParse, MixedNumbersAndRanges) {
    auto set = LineRangeSet::parse("5-12,18,22-30");
    EXPECT_EQ(set.lineCount(), 8 + 1 + 9);  // 8 + 1 + 9 = 18
    EXPECT_TRUE(set.contains(5));
    EXPECT_TRUE(set.contains(18));
    EXPECT_TRUE(set.contains(25));
    EXPECT_FALSE(set.contains(15));
    EXPECT_FALSE(set.contains(20));
}

TEST(LineRangeSetParse, OverlappingRanges) {
    auto set = LineRangeSet::parse("5-10,8-15");
    // Should merge into 5-15
    EXPECT_EQ(set.lineCount(), 11);
    EXPECT_TRUE(set.contains(5));
    EXPECT_TRUE(set.contains(12));
    EXPECT_TRUE(set.contains(15));
}

TEST(LineRangeSetParse, AdjacentRanges) {
    auto set = LineRangeSet::parse("1-3,4-6");
    // Should merge into 1-6
    EXPECT_EQ(set.lineCount(), 6);
    EXPECT_TRUE(set.contains(1));
    EXPECT_TRUE(set.contains(4));
    EXPECT_TRUE(set.contains(6));
}

TEST(LineRangeSetParse, SpacesAround) {
    auto set = LineRangeSet::parse(" 5-10 , 15 , 20-25 ");
    EXPECT_EQ(set.lineCount(), 6 + 1 + 6);  // 6 + 1 + 6 = 13
    EXPECT_TRUE(set.contains(5));
    EXPECT_TRUE(set.contains(15));
    EXPECT_TRUE(set.contains(25));
}

TEST(LineRangeSetParse, DuplicateNumbers) {
    auto set = LineRangeSet::parse("5,5,5");
    EXPECT_EQ(set.lineCount(), 1);
    EXPECT_TRUE(set.contains(5));
}

// ─── Serialization Tests ──────────────────────────────────────────────────

TEST(LineRangeSetSerialize, SingleNumber) {
    auto set = LineRangeSet::parse("42");
    EXPECT_EQ(set.toString(), "42");
}

TEST(LineRangeSetSerialize, Range) {
    auto set = LineRangeSet::parse("5-12");
    EXPECT_EQ(set.toString(), "5-12");
}

TEST(LineRangeSetSerialize, Mixed) {
    auto set = LineRangeSet::parse("5-12,18,22-30");
    EXPECT_EQ(set.toString(), "5-12,18,22-30");
}

TEST(LineRangeSetSerialize, Merged) {
    auto set = LineRangeSet::parse("5-10,8-15");
    // After merging: 5-15
    EXPECT_EQ(set.toString(), "5-15");
}

// ─── Round-Trip Tests ───────────────────────────────────────────────────────

TEST(LineRangeSetRoundTrip, Complex) {
    std::string input = "1-5,10,15-20,25,30-35";
    auto set = LineRangeSet::parse(input);
    std::string output = set.toString();
    auto set2 = LineRangeSet::parse(output);
    
    EXPECT_EQ(set.lineCount(), set2.lineCount());
    EXPECT_EQ(set.toString(), set2.toString());
}

// ─── toLines Tests ─────────────────────────────────────────────────────────

TEST(LineRangeSetToLines, Simple) {
    auto set = LineRangeSet::parse("1-3,5");
    auto lines = set.toLines();
    EXPECT_EQ(lines.size(), 4);
    EXPECT_EQ(lines[0], 1);
    EXPECT_EQ(lines[1], 2);
    EXPECT_EQ(lines[2], 3);
    EXPECT_EQ(lines[3], 5);
}

// ─── Edge Cases ────────────────────────────────────────────────────────────

TEST(LineRangeSetEdgeCase, SingleLineRange) {
    auto set = LineRangeSet::parse("5-5");
    EXPECT_EQ(set.lineCount(), 1);
    EXPECT_EQ(set.toString(), "5");
}

TEST(LineRangeSetEdgeCase, NegativeThrows) {
    // Negative numbers throw std::invalid_argument from stoi
    EXPECT_THROW(LineRangeSet::parse("-5,10"), std::invalid_argument);
}

TEST(LineRangeSetOps, Intersect) {
    auto a = LineRangeSet::parse("1-5,10-20");
    auto b = LineRangeSet::parse("3-7,12,18-25");

    EXPECT_EQ(a.intersect(b).toString(), "3-5,12,18-20");
}

TEST(LineRangeSetOps, Unite) {
    auto a = LineRangeSet::parse("1-5,10");
    auto b = LineRangeSet::parse("4-7,11-12");

    EXPECT_EQ(a.unite(b).toString(), "1-7,10-12");
}

TEST(LineRangeSetOps, Subtract) {
    auto a = LineRangeSet::parse("1-5,8-10,12");
    auto b = LineRangeSet::parse("3-8,12");

    EXPECT_EQ(a.subtract(b).toString(), "1-2,9-10");
}

TEST(LineRangeSetOps, FromLines) {
    auto set = LineRangeSet::fromLines({5, 3, 4, 10, 11, 11});

    EXPECT_EQ(set.toString(), "3-5,10-11");
}
