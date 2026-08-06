#include <RTEAutomation/Flasher.h>

#include <RTEAutomation/Mcp2221Gpio.h>
#include <RTEAutomation/Platform.h>
#include <RTEAutomation/ProcessRunner.h>
#include <inverter_protocol/host/uart_transport.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace RTEAutomation {
namespace {

void Notify(const FlashCallback& callback, FlashPhase phase,
            std::string message, int percent = -1) {
    if (callback) callback(FlashEvent{phase, percent, std::move(message)});
}

std::optional<fs::path> Existing(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return path;
    return std::nullopt;
}

int ParsePercent(const std::string& line) {
    const auto percent = line.find_last_of('%');
    if (percent == std::string::npos) return -1;
    auto begin = percent;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(line[begin - 1]))) --begin;
    if (begin == percent) return -1;
    return std::clamp(std::atoi(line.substr(begin, percent - begin).c_str()), 0, 100);
}

void Drain(const std::string& port, const FlashCallback& callback) {
    Notify(callback, FlashPhase::Drain, "Draining serial port");
    ivp::SerialPort serial;
    if (!serial.open(port, 230400)) {
        Notify(callback, FlashPhase::Drain, "Serial drain skipped; port was unavailable");
        return;
    }
    std::array<std::uint8_t, 4096> bytes{};
    for (int i = 0; i < 20; ++i) {
        serial.read(bytes.data(), static_cast<int>(bytes.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    serial.close();
}

}  // namespace

const char* FlashPhaseName(FlashPhase phase) {
    switch (phase) {
        case FlashPhase::Drain: return "drain";
        case FlashPhase::EnterBootloader: return "enter-bootloader";
        case FlashPhase::Program: return "program";
        case FlashPhase::Verify: return "verify";
        case FlashPhase::ExitBootloader: return "exit-bootloader";
        case FlashPhase::Complete: return "complete";
    }
    return "unknown";
}

fs::path FindStm32Programmer() {
    if (const char* configured = std::getenv("RTE_STM32_PROGRAMMER_CLI"))
        if (auto found = Existing(configured)) return *found;
    if (auto found = FindExecutableOnPath("STM32_Programmer_CLI")) return *found;
    std::vector<fs::path> candidates;
#ifdef _WIN32
    if (const char* files = std::getenv("PROGRAMFILES"))
        candidates.emplace_back(fs::path(files) / "STMicroelectronics" / "STM32Cube"
            / "STM32CubeProgrammer" / "bin" / "STM32_Programmer_CLI.exe");
#elif defined(__APPLE__)
    candidates.emplace_back("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI");
    candidates.emplace_back("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_Programmer_CLI");
    candidates.emplace_back("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
#else
    candidates.emplace_back("/opt/st/stm32cubeclt/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
    candidates.emplace_back("/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
#endif
    for (const auto& candidate : candidates)
        if (auto found = Existing(candidate)) return *found;
    return {};
}

FlashResult FlashFirmware(const FlashOptions& options, FlashCallback callback) {
    std::error_code ec;
    if (!fs::is_regular_file(options.firmware, ec))
        return {false, "firmware file not found: " + options.firmware.string()};
    if (options.serialPort.empty()) return {false, "serial port is required"};
    fs::path programmer = options.programmer.empty() ? FindStm32Programmer() : options.programmer;
    if (programmer.empty() || !fs::is_regular_file(programmer, ec))
        return {false, "STM32_Programmer_CLI was not found"};
    Drain(options.serialPort, callback);
    if (options.autoGpio) {
        Notify(callback, FlashPhase::EnterBootloader,
               "Entering bootloader using native MCP2221A GPIO control");
        const auto enter = ControlMcp2221Gpio(Mcp2221GpioAction::EnterBootloader);
        if (!enter.success) {
            return {false, "automatic MCP2221A bootloader entry failed: " + enter.error
                + ". Connect the MCP2221A and check USB permissions, or explicitly use "
                  "--manual-boot"};
        }
    } else {
        Notify(callback, FlashPhase::EnterBootloader,
               "Enter bootloader manually: BOOT0 high, then reset");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    Drain(options.serialPort, callback);

    ProcessSpec spec;
    spec.executable = programmer;
    spec.arguments = {"-c", "port=" + options.serialPort, "br=230400", "-w",
                      options.firmware.string(), "0x08000000", "-v"};
    bool verify = false;
    const auto process = RunProcess(spec, [&](const std::string& line) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.find("verif") != std::string::npos) verify = true;
        Notify(callback, verify ? FlashPhase::Verify : FlashPhase::Program,
               line, ParsePercent(line));
    });
    Mcp2221GpioResult gpioResult{true, {}};
    if (options.autoGpio) {
        Notify(callback, FlashPhase::ExitBootloader, "Releasing boot pins");
        gpioResult = ControlMcp2221Gpio(
            process.started && process.exitCode == 0
                ? Mcp2221GpioAction::StartApplication
                : Mcp2221GpioAction::ReleasePins);
        if (gpioResult.success && process.started && process.exitCode == 0)
            gpioResult = ControlMcp2221Gpio(Mcp2221GpioAction::ReleasePins);
        if (!gpioResult.success) {
            Notify(callback, FlashPhase::ExitBootloader,
                   "MCP2221A GPIO cleanup failed: " + gpioResult.error);
        }
    }
    if (!process.started || process.exitCode != 0)
        return {false, process.error.empty() ? "programmer failed" : process.error};
    if (!gpioResult.success)
        return {false, "firmware was programmed, but the application could not be restarted: "
            + gpioResult.error};
    Notify(callback, FlashPhase::Complete, "Firmware flash complete", 100);
    return {true, {}};
}

}  // namespace RTEAutomation
