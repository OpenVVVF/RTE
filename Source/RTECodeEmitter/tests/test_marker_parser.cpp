#include <gtest/gtest.h>

#include "../src/MarkerParser.h"

using namespace RTECodeEmitter;

TEST(MarkerParser, ParsesValidStateMarker) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: app_loop state", 7);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->domain, "app_loop");
    EXPECT_EQ(marker->section, "state");
    EXPECT_EQ(marker->lineNumber, 7);
}

TEST(MarkerParser, ParsesValidInitMarker) {
    auto marker = MarkerParser::ParseLine("    // RTE_EMIT: tim_isr init", 3);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->domain, "tim_isr");
    EXPECT_EQ(marker->section, "init");
    EXPECT_EQ(marker->lineNumber, 3);
}

TEST(MarkerParser, ParsesValidStepMarkerWithTrailingWhitespace) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: app_loop step  \n", 0);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->domain, "app_loop");
    EXPECT_EQ(marker->section, "step");
}

TEST(MarkerParser, SectionIsCaseInsensitive) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: app_loop STEP", 0);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->section, "step");
}

TEST(MarkerParser, RejectsUnknownSection) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: app_loop foo", 0);
    EXPECT_FALSE(marker.has_value());
}

TEST(MarkerParser, RejectsMissingSection) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: app_loop", 0);
    EXPECT_FALSE(marker.has_value());
}

TEST(MarkerParser, RejectsMissingDomain) {
    auto marker = MarkerParser::ParseLine("// RTE_EMIT: step", 0);
    EXPECT_FALSE(marker.has_value());
}

TEST(MarkerParser, RejectsPlainComment) {
    auto marker = MarkerParser::ParseLine("// TODO: fix this", 0);
    EXPECT_FALSE(marker.has_value());
}
