#include "Emitter.h"
#include <RTELogger/Logger.h>

#include <iostream>
#include <string_view>
#include <vector>

namespace {

void PrintUsage(std::string_view program) {
    std::cerr << "Usage: " << program
              << " --base-src <dir> --graph <file> --output <dir> [options]\n"
                 "\n"
                 "Options:\n"
                 "  --base-src <dir>        Base firmware source directory (required)\n"
                 "  --graph <file>          NodeAPI graph JSON file (required)\n"
                 "  --output <dir>          Output directory (required)\n"
                 "  --templates <dir>       Node template directory (optional)\n"
                 "  --generated-dir <name>  Generated subdirectory name (default: generated)\n"
                 "  --state-variable <name> Top-level state variable name (default: appState)\n"
                 "  --verbosity <level>     error|warning|info|debug|trace (default: info)\n"
                 "  --dry-run               Scan and log without writing files\n"
                 "  --help                  Show this message\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    RTECodeEmitter::EmitterOptions options;
    RTECodeEmitter::LogLevel logLevel = RTECodeEmitter::LogLevel::Info;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto NextArg = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--base-src") {
            options.baseSrc = NextArg("--base-src");
        } else if (arg == "--graph") {
            options.graphPath = NextArg("--graph");
        } else if (arg == "--output") {
            options.outputDir = NextArg("--output");
        } else if (arg == "--templates") {
            options.templatesDir = NextArg("--templates");
        } else if (arg == "--generated-dir") {
            options.generatedDirName = NextArg("--generated-dir");
        } else if (arg == "--state-variable") {
            options.stateVariable = NextArg("--state-variable");
        } else if (arg == "--verbosity") {
            logLevel = RTECodeEmitter::Logger::ParseLevel(NextArg("--verbosity"));
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    RTECodeEmitter::Logger logger(logLevel);
    RTECodeEmitter::Emitter emitter(logger);

    if (!emitter.Run(options)) {
        return 1;
    }

    return 0;
}
