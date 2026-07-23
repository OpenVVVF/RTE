#pragma once

#include <RTELogger/Logger.h>

#include <filesystem>
#include <string>

namespace RTECodeEmitter {

struct EmitterOptions {
    std::filesystem::path baseSrc;
    std::filesystem::path graphPath;
    std::filesystem::path outputDir;
    std::string generatedDirName = "generated";
    std::string stateVariable = "appState";
    LogLevel verbosity = LogLevel::Info;
    bool dryRun = false;
};

class Emitter {
public:
    explicit Emitter(const Logger& logger);

    bool Run(const EmitterOptions& options) const;

private:
    const Logger& logger_;
};

}  // namespace RTECodeEmitter
