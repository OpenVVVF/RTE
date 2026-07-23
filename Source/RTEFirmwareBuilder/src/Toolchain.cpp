#include "Toolchain.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace RTEFirmwareBuilder {

namespace {

std::vector<std::filesystem::path> SplitPath() {
    std::vector<std::filesystem::path> dirs;
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return dirs;

    std::string segment;
    std::istringstream iss(pathEnv);
    while (std::getline(iss, segment, ':')) {
        if (!segment.empty()) {
            dirs.emplace_back(segment);
        }
    }
    return dirs;
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
    dirs.emplace_back("/usr/bin");
    dirs.emplace_back("/usr/local/bin");
    dirs.emplace_back("/opt/gcc-arm-none-eabi/bin");

    AddProjectLocalToolchains(fwSrc, dirs);

    // STM32CubeCLT bundles its own ARM GCC under /opt/st/.
    try {
        if (std::filesystem::is_directory("/opt/st")) {
            for (const auto& entry : std::filesystem::directory_iterator("/opt/st")) {
                if (!entry.is_directory()) continue;
                auto candidate = entry.path() / "GNU-Tools-ARM-Embedded" / "bin";
                if (std::filesystem::is_directory(candidate)) {
                    dirs.push_back(candidate);
                }
            }
        }
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
                                    RTECodeEmitter::Logger& logger) {
    auto dir = FindToolchainDir(GccCandidateDirs(fwSrc), "arm-none-eabi-gcc", "arm-none-eabi-g++");
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
                                           RTECodeEmitter::Logger& logger) {
    auto dir = FindToolchainDir(StarmClangCandidateDirs(), "starm-clang", "starm-clang++");
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
    RTECodeEmitter::Logger& logger) {
    // Use an absolute path so the project-root walk can traverse up from relative
    // firmware directories like Images/Gen6FW.
    std::filesystem::path absFwSrc = std::filesystem::absolute(fwSrc);

    if (mode == "gcc") {
        return TryGcc(absFwSrc, logger);
    }
    if (mode == "starm-clang") {
        return TryStarmClang(absFwSrc, logger);
    }
    if (mode == "auto") {
        if (auto gcc = TryGcc(absFwSrc, logger)) return gcc;
        if (auto starm = TryStarmClang(absFwSrc, logger)) return starm;
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
