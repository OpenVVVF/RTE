#include "Builder.h"

#include "Emitter.h"

#include <RTEAutomation/Platform.h>
#include <RTEAutomation/ProcessRunner.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace RTEFirmwareBuilder {

namespace {

std::optional<std::filesystem::path> FindExecutableInPath(const std::string& name) {
    return RTEAutomation::FindExecutableOnPath(name);
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

Builder::Builder(RTECodeEmitter::Logger& logger) : logger_(logger) {}

bool Builder::Run(const BuilderOptions& options) {
    if (!ValidateOptions(options)) return false;

    std::filesystem::path effectiveFwSrc = options.fwSrc;

    if (options.graphPath) {
        if (!RunEmitter(options, effectiveFwSrc)) return false;
    }

    auto toolchain = DetectToolchain(
        effectiveFwSrc, options.toolchainMode, logger_,
        options.baseSrc.value_or(effectiveFwSrc));
    if (!toolchain) {
        logger_.Error("No usable ARM toolchain found. The firmware needs both "
                      "arm-none-eabi-gcc and arm-none-eabi-g++. Options:\n"
                      "  1. Run scripts/install_stm32_toolchain.sh to download the xPack toolchain "
                      "     into a project-local .tools/ directory.\n"
                      "  2. Install via your distro package manager, e.g.:\n"
                      "       Debian/Ubuntu: sudo apt install gcc-arm-none-eabi\n"
                      "       Fedora:        sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++\n"
                      "       Arch:          sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib\n"
                      "  3. Use the xPack/Arm GNU toolchain and ensure its bin directory is in PATH.");
        return false;
    }

    if (options.clean && !options.dryRun) {
        logger_.Info("Cleaning build directory: " + options.buildDir.string());
        std::filesystem::remove_all(options.buildDir);
    }

    if (!Configure(options, effectiveFwSrc, *toolchain)) return false;
    if (!Build(options, effectiveFwSrc, *toolchain)) return false;
    if (!Verify(options, effectiveFwSrc, *toolchain)) return false;

    logger_.Info("Firmware build completed successfully.");
    return true;
}

bool Builder::ValidateOptions(const BuilderOptions& options) const {
    auto cmakeLists = options.fwSrc / "CMakeLists.txt";
    if (!std::filesystem::is_regular_file(cmakeLists)) {
        logger_.Error("Firmware source does not contain CMakeLists.txt: " +
                      options.fwSrc.string());
        return false;
    }

    if (options.graphPath || options.baseSrc || options.outputDir) {
        if (!options.graphPath || !options.baseSrc || !options.outputDir) {
            logger_.Error("--graph, --base-src, and --output must all be supplied together.");
            return false;
        }
        if (!std::filesystem::is_regular_file(*options.graphPath)) {
            logger_.Error("Graph file not found: " + options.graphPath->string());
            return false;
        }
        auto baseCmake = *options.baseSrc / "CMakeLists.txt";
        if (!std::filesystem::is_regular_file(baseCmake)) {
            logger_.Error("Base firmware source missing CMakeLists.txt: " +
                          options.baseSrc->string());
            return false;
        }
    }

    return true;
}

bool Builder::RunEmitter(const BuilderOptions& options,
                         std::filesystem::path& effectiveFwSrc) const {
    RTECodeEmitter::EmitterOptions emitterOptions;
    emitterOptions.baseSrc = *options.baseSrc;
    emitterOptions.graphPath = *options.graphPath;
    emitterOptions.outputDir = *options.outputDir;
    if (options.templatesDir) emitterOptions.templatesDir = *options.templatesDir;
    emitterOptions.verbosity = options.verbosity;
    emitterOptions.dryRun = options.dryRun;
    RTECodeEmitter::Emitter emitter(logger_);
    logger_.Info("Generating firmware source: " + emitterOptions.outputDir.string());
    if (!emitter.Run(emitterOptions)) {
        logger_.Error("RTECodeEmitter failed.");
        return false;
    }

    effectiveFwSrc = *options.outputDir;
    logger_.Info("Emitter produced firmware tree at: " + effectiveFwSrc.string());
    return true;
}

bool Builder::Configure(const BuilderOptions& options,
                        const std::filesystem::path& fwSrc,
                        const ToolchainInfo& toolchain) const {
    std::string generator = options.generator;

    if (ToLower(generator) == "ninja") {
        if (!FindExecutableInPath("ninja") && !FindExecutableInPath("ninja-build")) {
            logger_.Warning("Ninja generator requested but ninja/ninja-build not found; "
                            "falling back to Unix Makefiles.");
            generator = "Unix Makefiles";
        }
    }

    std::vector<std::string> args;
    args.push_back("cmake");
    args.push_back("-S");
    args.push_back(fwSrc.string());
    args.push_back("-B");
    args.push_back(options.buildDir.string());
    args.push_back("-G");
    args.push_back(generator);
    args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain.toolchainFile.string());
    args.push_back("-DCMAKE_BUILD_TYPE=" + options.buildType);

    /* The emitted tree lives outside the repo: point the firmware at the
     * shared InverterProtocol library explicitly (default in the firmware
     * CMake only covers in-tree builds). */
    {
        std::error_code ec;
        const auto ivpCore = std::filesystem::weakly_canonical(
            options.fwSrc / ".." / ".." / "Lib" / "InverterProtocol", ec);
        if (!ec && std::filesystem::is_directory(ivpCore)) {
            args.push_back("-DIVP_CORE_DIR=" + ivpCore.string());
        }
    }

    return RunCommand("Configuring firmware", args, options.dryRun, toolchain.compilerDir);
}

bool Builder::Build(const BuilderOptions& options,
                    const std::filesystem::path& /*fwSrc*/,
                    const ToolchainInfo& toolchain) const {
    unsigned int concurrency = std::max(1u, std::thread::hardware_concurrency());

    std::vector<std::string> args;
    args.push_back("cmake");
    args.push_back("--build");
    args.push_back(options.buildDir.string());
    args.push_back("--target");
    args.push_back("STM32CubeMX");
    args.push_back("--parallel");
    args.push_back(std::to_string(concurrency));

    return RunCommand("Building firmware", args, options.dryRun, toolchain.compilerDir);
}

bool Builder::Verify(const BuilderOptions& options,
                     const std::filesystem::path& /*fwSrc*/,
                     const ToolchainInfo& toolchain) const {
    auto elf = options.buildDir / "STM32CubeMX.elf";
    auto bin = options.buildDir / "STM32CubeMX.bin";

    if (!std::filesystem::is_regular_file(elf)) {
        logger_.Error("Build did not produce expected ELF: " + elf.string());
        return false;
    }
    if (!std::filesystem::is_regular_file(bin)) {
        logger_.Error("Build did not produce expected BIN: " + bin.string());
        return false;
    }

    logger_.Info("ELF: " + elf.string());
    logger_.Info("BIN: " + bin.string());
    logger_.Info("ELF size: " + std::to_string(std::filesystem::file_size(elf)) + " bytes");
    logger_.Info("BIN size: " + std::to_string(std::filesystem::file_size(bin)) + " bytes");

    if (auto sizeTool = FindSizeTool(toolchain)) {
        std::vector<std::string> args;
        args.push_back(sizeTool->string());
        args.push_back(elf.string());
        RunCommand("Firmware size summary", args, options.dryRun, toolchain.compilerDir);
    } else {
        logger_.Debug("Size tool not available; skipping size summary.");
    }

    return true;
}

bool Builder::RunCommand(const std::string& description,
                         const std::vector<std::string>& args,
                         bool dryRun,
                         const std::filesystem::path& pathPrefix) const {
    if (args.empty()) return false;
    RTEAutomation::ProcessSpec spec;
    spec.executable = args.front();
    spec.arguments.assign(args.begin() + 1, args.end());
    if (!pathPrefix.empty()) {
        const char* oldPath = std::getenv("PATH");
        spec.environment["PATH"] = pathPrefix.string()
            + RTEAutomation::PathListSeparator() + (oldPath ? oldPath : "");
    }
    logger_.Info(description + ": " + RTEAutomation::FormatCommandForDisplay(spec));

    if (dryRun) {
        logger_.Info("[dry-run] skipping execution");
        return true;
    }

    std::vector<std::string> output;
    const auto result = RTEAutomation::RunProcess(
        spec, [&](const std::string& line) { output.push_back(line); });
    if (!result.started || result.exitCode != 0) {
        logger_.Error("Command failed: " + result.error + ": "
                      + RTEAutomation::FormatCommandForDisplay(spec));
        logger_.Error("---- full output ----");
        for (const auto& line : output) {
            logger_.Error(line);
        }
        logger_.Error("---------------------");
        return false;
    }

    for (const auto& line : output) {
        logger_.Debug(line);
    }
    return true;
}

std::optional<std::filesystem::path> Builder::FindRTECodeEmitter() const {
    const auto self = RTEAutomation::ExecutablePath();
    if (self.empty()) return std::nullopt;

    auto dir = self.parent_path();

    // Same directory as this executable.
    auto sameDir = dir / RTEAutomation::ExecutableName("RTECodeEmitter");
    if (std::filesystem::is_regular_file(sameDir)) return sameDir;

    // Sibling source directory layout: Source/RTEFirmwareBuilder and Source/RTECodeEmitter.
    auto siblingDir = dir.parent_path() / "RTECodeEmitter"
        / RTEAutomation::ExecutableName("RTECodeEmitter");
    if (std::filesystem::is_regular_file(siblingDir)) return siblingDir;

    // Finally, fall back to PATH.
    if (auto fromPath = FindExecutableInPath("RTECodeEmitter")) return fromPath;

    return std::nullopt;
}

std::optional<std::filesystem::path> Builder::FindSizeTool(
    const ToolchainInfo& toolchain) const {
    if (toolchain.type == ToolchainType::Gcc) {
        auto size = toolchain.compilerDir / "arm-none-eabi-size";
        if (std::filesystem::is_regular_file(size)) return size;
    }

    // Fall back to PATH.
    if (toolchain.type == ToolchainType::Gcc) {
        if (auto candidate = FindExecutableInPath("arm-none-eabi-size")) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace RTEFirmwareBuilder
