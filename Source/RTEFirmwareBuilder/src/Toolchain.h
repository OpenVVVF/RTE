#pragma once

#include <RTELogger/Logger.h>

#include <filesystem>
#include <optional>
#include <string>

namespace RTEFirmwareBuilder {

enum class ToolchainType {
    Gcc,
    StarmClang,
    Custom,
};

struct ToolchainInfo {
    ToolchainType type;
    std::filesystem::path toolchainFile;
    std::filesystem::path compilerDir;
    std::string name;
};

std::optional<ToolchainInfo> DetectToolchain(
    const std::filesystem::path& fwSrc,
    const std::string& mode,
    RTECodeEmitter::Logger& logger);

}  // namespace RTEFirmwareBuilder
