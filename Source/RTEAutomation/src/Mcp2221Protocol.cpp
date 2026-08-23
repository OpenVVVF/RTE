#include "Mcp2221Protocol.h"

#include <chrono>
#include <optional>

namespace RTEAutomation {
namespace {

constexpr unsigned char kSetGpio = 0x50;
constexpr unsigned char kSetSram = 0x60;
constexpr unsigned char kGetSram = 0x61;

bool SendChecked(const Mcp2221Protocol::Exchange& exchange,
                 const Mcp2221Protocol::Report& request,
                 Mcp2221Protocol::Report& response, std::string& error) {
    if (!exchange(request, response, error)) return false;
    if (response[0] != request[0]) {
        error = "MCP2221A returned a response for the wrong command";
        return false;
    }
    if (response[1] != 0) {
        error = "MCP2221A rejected command 0x";
        constexpr char hex[] = "0123456789abcdef";
        error.push_back(hex[(request[0] >> 4) & 0x0f]);
        error.push_back(hex[request[0] & 0x0f]);
        return false;
    }
    return true;
}

Mcp2221Protocol::Report GpioValues(std::optional<bool> boot0,
                                   std::optional<bool> reset) {
    Mcp2221Protocol::Report report{};
    report[0] = kSetGpio;
    if (boot0) {
        report[2] = 1;
        report[3] = *boot0 ? 1 : 0;
    }
    if (reset) {
        report[6] = 1;
        report[7] = *reset ? 1 : 0;
    }
    return report;
}

Mcp2221Protocol::Report ConfigurePins(const Mcp2221Protocol::Report& settings,
                                      bool outputs, bool boot0, bool reset) {
    Mcp2221Protocol::Report report{};
    report[0] = kSetSram;

    // Preserve ADC/DAC configuration while changing GPIO designation. The Get
    // SRAM response encodes these fields differently from Set SRAM.
    report[3] = static_cast<unsigned char>(0x80 | ((settings[6] >> 5) & 0x07));
    report[4] = static_cast<unsigned char>(0x80 | (settings[6] & 0x1f));
    report[5] = static_cast<unsigned char>(0x80 | ((settings[7] >> 2) & 0x07));
    report[7] = 0x80;

    const unsigned char direction = outputs ? 0x00 : 0x08;
    report[8] = static_cast<unsigned char>(direction | (boot0 ? 0x10 : 0x00));
    report[9] = static_cast<unsigned char>(direction | (reset ? 0x10 : 0x00));
    report[10] = settings[24];
    report[11] = settings[25];
    return report;
}

}  // namespace

namespace Mcp2221Protocol {

bool Execute(Mcp2221GpioAction action, const Exchange& exchange,
             const Delay& delay, std::string& error) {
    Report request{};
    Report response{};
    request[0] = kGetSram;
    if (!SendChecked(exchange, request, response, error)) return false;

    const bool release = action == Mcp2221GpioAction::ReleasePins;
    request = ConfigurePins(response, !release, false, !release);
    if (!SendChecked(exchange, request, response, error)) return false;
    if (release) return true;
    delay(std::chrono::milliseconds(10));

    if (action == Mcp2221GpioAction::EnterBootloader) {
        request = GpioValues(true, true);
        if (!SendChecked(exchange, request, response, error)) return false;
        delay(std::chrono::milliseconds(50));
        request = GpioValues(true, false);
        if (!SendChecked(exchange, request, response, error)) return false;
        delay(std::chrono::milliseconds(50));
        request = GpioValues(true, true);
        if (!SendChecked(exchange, request, response, error)) return false;
        delay(std::chrono::milliseconds(250));
    } else {
        request = GpioValues(false, false);
        if (!SendChecked(exchange, request, response, error)) return false;
        delay(std::chrono::milliseconds(50));
        request = GpioValues(std::nullopt, true);
        if (!SendChecked(exchange, request, response, error)) return false;
        delay(std::chrono::milliseconds(100));
    }
    return true;
}

}  // namespace Mcp2221Protocol
}  // namespace RTEAutomation
