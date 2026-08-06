#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace RTEAutomation {

enum class FlashPhase { Drain, EnterBootloader, Program, Verify, ExitBootloader, Complete };

struct FlashOptions {
    std::filesystem::path firmware;
    std::string serialPort;
    std::filesystem::path programmer;
    std::filesystem::path gpioHelper;
    std::filesystem::path python;
    bool autoGpio = true;
};

struct FlashEvent {
    FlashPhase phase = FlashPhase::Drain;
    int percent = -1;
    std::string message;
};

struct FlashResult { bool success = false; std::string error; };
using FlashCallback = std::function<void(const FlashEvent&)>;

std::filesystem::path FindStm32Programmer();
std::filesystem::path FindPythonInterpreter();
std::filesystem::path FindGpioHelper();
const char* FlashPhaseName(FlashPhase phase);
FlashResult FlashFirmware(const FlashOptions& options, FlashCallback callback = {});

}  // namespace RTEAutomation
