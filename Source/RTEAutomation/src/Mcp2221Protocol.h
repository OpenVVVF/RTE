#pragma once

#include <RTEAutomation/Mcp2221Gpio.h>

#include <array>
#include <chrono>
#include <functional>
#include <string>

namespace RTEAutomation::Mcp2221Protocol {

using Report = std::array<unsigned char, 64>;
using Exchange = std::function<bool(const Report&, Report&, std::string&)>;
using Delay = std::function<void(std::chrono::milliseconds)>;

bool Execute(Mcp2221GpioAction action, const Exchange& exchange,
             const Delay& delay, std::string& error);

}  // namespace RTEAutomation::Mcp2221Protocol
