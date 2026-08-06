#include <RTEAutomation/ProcessRunner.h>

#include <gtest/gtest.h>

#ifndef RTE_CLI_PATH
#error RTE_CLI_PATH must name the rte executable
#endif

TEST(RteCli, ReportsStructuredVersion) {
    RTEAutomation::ProcessSpec spec;
    spec.executable = RTE_CLI_PATH;
    spec.arguments = {"--format", "jsonl", "--version"};
    std::vector<std::string> lines;
    const auto result = RTEAutomation::RunProcess(
        spec, [&](const std::string& line) { lines.push_back(line); });
    ASSERT_TRUE(result.started);
    ASSERT_EQ(result.exitCode, 0);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("\"event\":\"version\""), std::string::npos);
}
