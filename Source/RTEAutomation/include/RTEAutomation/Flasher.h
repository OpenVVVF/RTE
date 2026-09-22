#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace RTEAutomation {

enum class FlashPhase { Drain, EnterBootloader, Program, Verify, ExitBootloader, Complete };
enum class FlashTarget { Main, Coprocessor };

struct Gen7BridgePorts {
    std::string bridge;
    std::string control;
};

struct Gen7ControlStatus {
    bool success = false;
    std::string line;
    std::string error;
};

struct FlashOptions {
    std::filesystem::path firmware;
    std::string serialPort;
    std::string controlPort;
    std::filesystem::path programmer;
    FlashTarget target = FlashTarget::Main;
    bool automaticBoot = true;
    unsigned attempts = 3;
};

struct FlashEvent {
    FlashPhase phase = FlashPhase::Drain;
    int percent = -1;
    std::string message;
};

struct FlashResult { bool success = false; std::string error; };
using FlashCallback = std::function<void(const FlashEvent&)>;

std::filesystem::path FindStm32Programmer();
Gen7BridgePorts DiscoverGen7BridgePorts(const std::string& preferredBridge = {});
Gen7ControlStatus ReadGen7ControlStatus(const std::string& controlPort);
const char* FlashPhaseName(FlashPhase phase);
FlashResult FlashFirmware(const FlashOptions& options, FlashCallback callback = {});

}  // namespace RTEAutomation
