#include <RTEAutomation/Mcp2221Gpio.h>

namespace RTEAutomation {

Mcp2221GpioResult ControlMcp2221Gpio(Mcp2221GpioAction) {
    return {false, "native MCP2221A GPIO support was disabled in this build"};
}

}  // namespace RTEAutomation
