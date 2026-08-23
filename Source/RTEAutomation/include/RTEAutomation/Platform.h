#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RTEAutomation {

std::filesystem::path ExecutablePath();
std::vector<std::filesystem::path> PathDirectories();
std::optional<std::filesystem::path> FindExecutableOnPath(
    const std::string& name);
std::string ExecutableName(std::string name);
char PathListSeparator();

}  // namespace RTEAutomation
