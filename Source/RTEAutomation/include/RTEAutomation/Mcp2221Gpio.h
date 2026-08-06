#pragma once

#include <string>

namespace RTEAutomation {

enum class Mcp2221GpioAction {
    EnterBootloader,
    StartApplication,
    ReleasePins,
};

struct Mcp2221GpioResult {
    bool success = false;
    std::string error;
};

// Controls the first MCP2221A found on USB (VID 04d8, PID 00dd). GP0 drives
// BOOT0 and GP1 drives active-low NRST.
Mcp2221GpioResult ControlMcp2221Gpio(Mcp2221GpioAction action);

}  // namespace RTEAutomation
