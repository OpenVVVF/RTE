#include "Toolchain.h"

#include <RTEAutomation/Platform.h>

#include <cstdlib>
#include <vector>

namespace RTEFirmwareBuilder {

namespace {

std::vector<std::filesystem::path> SplitPath() {
    return RTEAutomation::PathDirectories();
}

void AddProjectLocalToolchains(const std::filesystem::path& fwSrc,
                               std::vector<std::filesystem::path>& extras) {
    std::filesystem::path dir = fwSrc;
    try {
        for (int i = 0; i < 8 && !dir.empty(); ++i, dir = dir.parent_path()) {
            auto toolsDir = dir / ".tools";
            if (!std::filesystem::is_directory(toolsDir)) continue;

            for (const auto& entry : std::filesystem::directory_iterator(toolsDir)) {
                if (!entry.is_directory()) continue;
                auto name = entry.path().filename().string();
                if (name.find("xpack-arm-none-eabi-gcc-") == std::string::npos) continue;
                auto binDir = entry.path() / "bin";
                if (std::filesystem::is_directory(binDir)) {
                    extras.push_back(binDir);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Ignore permission or traversal errors.
    }
}

std::vector<std::filesystem::path> GccCandidateDirs(const std::filesystem::path& fwSrc) {
    std::vector<std::filesystem::path> dirs = SplitPath();
#ifndef _WIN32
    dirs.emplace_back("/usr/bin");
    dirs.emplace_back("/usr/local/bin");
    dirs.emplace_back("/opt/gcc-arm-none-eabi/bin");
#endif

    AddProjectLocalToolchains(fwSrc, dirs);

    // STM32CubeCLT bundles its own ARM GCC under /opt/st/.
    try {
#ifndef _WIN32
        if (std::filesystem::is_directory("/opt/st")) {
            for (const auto& entry : std::filesystem::directory_iterator("/opt/st")) {
                if (!entry.is_directory()) continue;
                auto candidate = entry.path() / "GNU-Tools-ARM-Embedded" / "bin";
                if (std::filesystem::is_directory(candidate)) {
                    dirs.push_back(candidate);
                }
            }
        }
#endif
    } catch (const std::filesystem::filesystem_error&) {
        // Ignore permission or traversal errors.
    }
    return dirs;
}

std::optional<std::filesystem::path> FindToolchainDir(
    const std::vector<std::filesystem::path>& dirs,
    const std::string& compilerName,
    const std::string& cppCompilerName) {
    for (const auto& dir : dirs) {
        if (std::filesystem::is_regular_file(dir / compilerName) &&
            std::filesystem::is_regular_file(dir / cppCompilerName)) {
            return dir;
        }
    }
    return std::nullopt;
}

std::optional<ToolchainInfo> TryGcc(const std::filesystem::path& fwSrc,
                                    const std::filesystem::path& toolSearchRoot,
                                    RTECodeEmitter::Logger& logger) {
    auto dir = FindToolchainDir(GccCandidateDirs(
                                    toolSearchRoot.empty() ? fwSrc : toolSearchRoot),
                                RTEAutomation::ExecutableName("arm-none-eabi-gcc"),
                                RTEAutomation::ExecutableName("arm-none-eabi-g++"));
    if (!dir) {
        logger.Debug("arm-none-eabi-gcc/g++ not found");
        return std::nullopt;
    }

    ToolchainInfo info;
    info.type = ToolchainType::Gcc;
    info.toolchainFile = fwSrc / "cmake" / "gcc-arm-none-eabi.cmake";
    info.compilerDir = *dir;
    info.name = "gcc-arm-none-eabi";

    if (!std::filesystem::is_regular_file(info.toolchainFile)) {
        logger.Warning("Found arm-none-eabi toolchain but toolchain file is missing: " +
                       info.toolchainFile.string());
        return std::nullopt;
    }

    logger.Info("Detected GCC ARM toolchain: " + (info.compilerDir / "arm-none-eabi-gcc").string());
    return info;
}

std::vector<std::filesystem::path> StarmClangCandidateDirs() {
    return SplitPath();
}

std::optional<ToolchainInfo> TryStarmClang(const std::filesystem::path& fwSrc,
                                           const std::filesystem::path&,
                                           RTECodeEmitter::Logger& logger) {
    auto dir = FindToolchainDir(StarmClangCandidateDirs(),
                                RTEAutomation::ExecutableName("starm-clang"),
                                RTEAutomation::ExecutableName("starm-clang++"));
    if (!dir) {
        logger.Debug("starm-clang/starm-clang++ not found");
        return std::nullopt;
    }

    ToolchainInfo info;
    info.type = ToolchainType::StarmClang;
    info.toolchainFile = fwSrc / "cmake" / "starm-clang.cmake";
    info.compilerDir = *dir;
    info.name = "starm-clang";

    if (!std::filesystem::is_regular_file(info.toolchainFile)) {
        logger.Warning("Found starm-clang toolchain but toolchain file is missing: " +
                       info.toolchainFile.string());
        return std::nullopt;
    }

    logger.Info("Detected ST ARM Clang toolchain: " + (info.compilerDir / "starm-clang").string());
    return info;
}

}  // namespace

std::optional<ToolchainInfo> DetectToolchain(
    const std::filesystem::path& fwSrc,
    const std::string& mode,
    RTECodeEmitter::Logger& logger,
    const std::filesystem::path& toolSearchRoot) {
    // Use an absolute path so the project-root walk can traverse up from relative
    // firmware directories like Images/Gen6FW.
    std::filesystem::path absFwSrc = std::filesystem::absolute(fwSrc);
    const std::filesystem::path absToolSearchRoot = toolSearchRoot.empty()
        ? absFwSrc : std::filesystem::absolute(toolSearchRoot);

    if (mode == "gcc") {
        return TryGcc(absFwSrc, absToolSearchRoot, logger);
    }
    if (mode == "starm-clang") {
        return TryStarmClang(absFwSrc, absToolSearchRoot, logger);
    }
    if (mode == "auto") {
        if (auto gcc = TryGcc(absFwSrc, absToolSearchRoot, logger)) return gcc;
        if (auto starm = TryStarmClang(absFwSrc, absToolSearchRoot, logger)) return starm;
        return std::nullopt;
    }

    // Treat any other value as a direct path to a toolchain file.
    std::filesystem::path customFile(mode);
    if (std::filesystem::is_regular_file(customFile)) {
        ToolchainInfo info;
        info.type = ToolchainType::Custom;
        info.toolchainFile = std::filesystem::absolute(customFile);
        info.compilerDir = std::filesystem::path{};
        info.name = "custom";
        logger.Info("Using custom toolchain file: " + info.toolchainFile.string());
        return info;
    }

    logger.Error("Toolchain file not found: " + mode);
    return std::nullopt;
}

}  // namespace RTEFirmwareBuilder
