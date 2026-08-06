#pragma once

#include "Toolchain.h"

#include <RTELogger/Logger.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RTEFirmwareBuilder {

struct BuilderOptions {
    std::filesystem::path fwSrc;
    std::filesystem::path buildDir;
    std::string toolchainMode = "auto";
    std::string buildType = "Debug";
    std::string generator = "Ninja";
    std::optional<std::filesystem::path> graphPath;
    std::optional<std::filesystem::path> baseSrc;
    std::optional<std::filesystem::path> outputDir;
    std::optional<std::filesystem::path> templatesDir;
    RTECodeEmitter::LogLevel verbosity = RTECodeEmitter::LogLevel::Info;
    bool dryRun = false;
    bool clean = false;
};

class Builder {
public:
    explicit Builder(RTECodeEmitter::Logger& logger);

    bool Run(const BuilderOptions& options);

private:
    RTECodeEmitter::Logger& logger_;

    bool ValidateOptions(const BuilderOptions& options) const;
    bool RunEmitter(const BuilderOptions& options,
                    std::filesystem::path& effectiveFwSrc) const;
    bool Configure(const BuilderOptions& options,
                   const std::filesystem::path& fwSrc,
                   const ToolchainInfo& toolchain) const;
    bool Build(const BuilderOptions& options,
               const std::filesystem::path& fwSrc,
               const ToolchainInfo& toolchain) const;
    bool Verify(const BuilderOptions& options,
                const std::filesystem::path& fwSrc,
                const ToolchainInfo& toolchain) const;

    bool RunCommand(const std::string& description,
                    const std::vector<std::string>& args,
                    bool dryRun,
                    const std::filesystem::path& pathPrefix = {}) const;
    std::optional<std::filesystem::path> FindRTECodeEmitter() const;
    std::optional<std::filesystem::path> FindSizeTool(const ToolchainInfo& toolchain) const;
};

}  // namespace RTEFirmwareBuilder
