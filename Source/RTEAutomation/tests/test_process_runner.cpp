#include <RTEAutomation/ProcessRunner.h>

#include <gtest/gtest.h>

TEST(ProcessRunner, CapturesMergedOutputAndExitCode) {
    RTEAutomation::ProcessSpec spec;
#ifdef _WIN32
    spec.executable = "cmd.exe";
    spec.arguments = {"/D", "/S", "/C", "echo(first&echo(second 1>&2"};
#else
    spec.executable = "/bin/sh";
    spec.arguments = {"-c", "printf 'first\\n'; printf 'second\\n' >&2"};
#endif
    std::vector<std::string> lines;
    const auto result = RTEAutomation::RunProcess(
        spec, [&](const std::string& line) { lines.push_back(line); });
    ASSERT_TRUE(result.started);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(lines, (std::vector<std::string>{"first", "second"}));
}
