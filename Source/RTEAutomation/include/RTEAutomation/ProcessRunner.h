#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace RTEAutomation {

struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::map<std::string, std::string> environment;
};

struct ProcessResult {
    bool started = false;
    int exitCode = -1;
    std::string error;
};

using ProcessOutput = std::function<void(const std::string&)>;

// Runs without a shell. stdout and stderr are merged and delivered a line at
// a time. Arguments are passed as individual values on every platform.
ProcessResult RunProcess(const ProcessSpec& spec, ProcessOutput output = {});

std::string FormatCommandForDisplay(const ProcessSpec& spec);

}  // namespace RTEAutomation
