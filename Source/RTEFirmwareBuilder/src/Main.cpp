#include "Builder.h"

#include <RTELogger/Logger.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void PrintUsage(std::string_view program) {
    std::cerr << "Usage: " << program
              << " --fw-src <dir> --build-dir <dir> [options]\n"
                 "\n"
                 "Required:\n"
                 "  --fw-src <dir>          Firmware source directory (must contain CMakeLists.txt)\n"
                 "  --build-dir <dir>       Directory for CMake build artifacts\n"
                 "\n"
                 "Build options:\n"
                 "  --toolchain <mode>      auto | gcc | starm-clang | <path> (default: auto)\n"
                 "  --build-type <type>     Debug | Release | RelWithDebInfo | MinSizeRel (default: Debug)\n"
                 "  --generator <gen>       CMake generator (default: Ninja, falls back to Makefiles)\n"
                 "  --clean                 Delete build-dir before configuring\n"
                 "\n"
                 "Emitter integration (optional):\n"
                 "  --graph <file>          NodeAPI graph JSON\n"
                 "  --base-src <dir>        Base firmware source for emitter\n"
                 "  --output <dir>          Output directory for emitted firmware\n"
                 "\n"
                 "Other options:\n"
                 "  --verbosity <level>     error | warning | info | debug | trace (default: info)\n"
                 "  --dry-run               Print commands without executing\n"
                 "  --help                  Show this message\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    RTEFirmwareBuilder::BuilderOptions options;
    RTECodeEmitter::LogLevel logLevel = RTECodeEmitter::LogLevel::Info;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto Next = [&](std::string_view name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(1);
            }
            return std::string(argv[++i]);
        };

        if (arg == "--fw-src") {
            options.fwSrc = Next("--fw-src");
        } else if (arg == "--build-dir") {
            options.buildDir = Next("--build-dir");
        } else if (arg == "--toolchain") {
            options.toolchainMode = Next("--toolchain");
        } else if (arg == "--build-type") {
            options.buildType = Next("--build-type");
        } else if (arg == "--generator") {
            options.generator = Next("--generator");
        } else if (arg == "--graph") {
            options.graphPath = Next("--graph");
        } else if (arg == "--base-src") {
            options.baseSrc = Next("--base-src");
        } else if (arg == "--output") {
            options.outputDir = Next("--output");
        } else if (arg == "--verbosity") {
            logLevel = RTECodeEmitter::Logger::ParseLevel(Next("--verbosity"));
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--clean") {
            options.clean = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    options.verbosity = logLevel;

    RTECodeEmitter::Logger logger(logLevel);
    RTEFirmwareBuilder::Builder builder(logger);

    if (!builder.Run(options)) {
        return 1;
    }
    return 0;
}
