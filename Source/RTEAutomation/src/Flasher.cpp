#include <RTEAutomation/Flasher.h>

#include <RTEAutomation/Platform.h>
#include <RTEAutomation/ProcessRunner.h>
#include <inverter_protocol/host/uart_transport.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
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

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string BoundedDiagnostic(const std::string& line) {
    constexpr std::size_t kMaxLength = 1024;
    if (line.size() <= kMaxLength) return line;
    return line.substr(0, kMaxLength) + "...";
}

bool IsDiagnostic(const std::string& line) {
    const std::string lower = Lower(line);
    return lower.find("error") != std::string::npos
        || lower.find("fail") != std::string::npos
        || lower.find("unable") != std::string::npos
        || lower.find("cannot") != std::string::npos
        || lower.find("not found") != std::string::npos
        || lower.find("timeout") != std::string::npos
        || lower.find("abort") != std::string::npos;
}

bool SamePort(const fs::path& left, const fs::path& right) {
    if (left == right) return true;
    std::error_code leftError, rightError;
    const auto a = fs::canonical(left, leftError);
    const auto b = fs::canonical(right, rightError);
    return !leftError && !rightError && a == b;
}

FlashResult ControlCommand(const std::string& port, const char* command) {
    ivp::SerialPort serial;
    if (!serial.open(port, 115200))
        return {false, "could not open coprocessor control port " + port};
    const std::string request = std::string(command) + "\r\n";
    if (!serial.write(reinterpret_cast<const std::uint8_t*>(request.data()),
                      static_cast<int>(request.size())) || !serial.drain())
        return {false, "could not send " + std::string(command) + " on " + port};
    std::string response;
    std::array<std::uint8_t, 256> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const std::string expected = std::string(command) + " OK";
    while (std::chrono::steady_clock::now() < deadline) {
        const int count = serial.read(bytes.data(), static_cast<int>(bytes.size()));
        if (count < 0) return {false, "control port read failed on " + port};
        if (count == 0) continue;
        response.append(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::size_t>(count));
        if (response.find(expected) != std::string::npos) return {true, {}};
        if (response.find("ERROR") != std::string::npos)
            return {false, "coprocessor rejected " + std::string(command) + ": " + response};
        if (response.size() > 2048) response.erase(0, response.size() - 1024);
    }
    return {false, "timed out waiting for " + expected + " on " + port
        + (response.empty() ? "" : ": " + response)};
}

std::optional<std::uint32_t> FirmwareEntry(const fs::path& firmware) {
    std::ifstream input(firmware, std::ios::binary);
    if (!input) return std::nullopt;
    std::array<unsigned char, 52> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() < 8) return std::nullopt;
    const auto u32 = [&](std::size_t offset) {
        return std::uint32_t(header[offset]) | (std::uint32_t(header[offset+1]) << 8)
             | (std::uint32_t(header[offset+2]) << 16) | (std::uint32_t(header[offset+3]) << 24);
    };
    const std::string ext = Lower(firmware.extension().string());
    if (ext == ".bin") return u32(4);
    if (ext == ".elf" && input.gcount() >= 52 && header[0] == 0x7f
        && header[1] == 'E' && header[2] == 'L' && header[3] == 'F'
        && header[4] == 1 && header[5] == 1) return u32(24);
    return std::nullopt;
}

std::string HexAddress(std::uint32_t address) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string value = "0x00000000";
    for (int i = 0; i < 8; ++i)
        value[9 - i] = digits[(address >> (i * 4)) & 0xfU];
    return value;
}

FlashResult RunProgrammer(const fs::path& programmer,
                          const std::vector<std::string>& arguments,
                          const FlashCallback& callback) {
    ProcessSpec spec;
    spec.executable = programmer;
    spec.arguments = arguments;
    bool verifying = false;
    std::string tail;
    std::string diagnostic;
    const auto result = RunProcess(spec, [&](const std::string& line) {
        if (Lower(line).find("verif") != std::string::npos) verifying = true;
        Notify(callback, verifying ? FlashPhase::Verify : FlashPhase::Program,
               line, ParsePercent(line));
        tail = BoundedDiagnostic(line);
        if (IsDiagnostic(line)) diagnostic = BoundedDiagnostic(line);
    });
    if (!result.started) return {false, result.error};
    if (result.exitCode != 0)
        return {false, "STM32CubeProgrammer exited with " + std::to_string(result.exitCode)
            + (!diagnostic.empty() ? ": " + diagnostic
                                   : tail.empty() ? "" : ": " + tail)};
    return {true, {}};
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
    candidates.emplace_back("/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
    if (const char* userHome = std::getenv("HOME"))
        candidates.emplace_back(fs::path(userHome) / "STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
    candidates.emplace_back("/opt/st/stm32cubeclt/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
    candidates.emplace_back("/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI");
#endif
    for (const auto& candidate : candidates)
        if (auto found = Existing(candidate)) return *found;
    return {};
}

Gen7BridgePorts DiscoverGen7BridgePorts(const std::string& preferredBridge) {
    Gen7BridgePorts ports;
#if defined(__linux__)
    fs::path directory = "/dev/serial/by-id";
    if (const char* override = std::getenv("RTE_SERIAL_BY_ID_DIR")) directory = override;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return ports;
    std::vector<Gen7BridgePorts> matches;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        const std::string filename = entry.path().filename().string();
        if (filename.find("usb-OpenVVVF_") != 0) continue;
        const auto marker = filename.find("if00");
        if (marker == std::string::npos) continue;
        std::string control = filename;
        control.replace(marker, 4, "if02");
        const fs::path controlPath = directory / control;
        if (!fs::exists(controlPath, ec)) continue;
        if (!preferredBridge.empty() && !SamePort(entry.path(), preferredBridge)) continue;
        matches.push_back({entry.path().string(), controlPath.string()});
    }
    if (matches.size() == 1) return matches.front();
#else
    (void)preferredBridge;
#endif
    return ports;
}

Gen7ControlStatus ReadGen7ControlStatus(const std::string& controlPort) {
    if (controlPort.empty()) return {false, {}, "Gen7 control port not found"};
    ivp::SerialPort serial;
    if (!serial.open(controlPort, 115200))
        return {false, {}, "could not open coprocessor control port " + controlPort};
    constexpr char command[] = "STATUS\r\n";
    if (!serial.write(reinterpret_cast<const std::uint8_t*>(command), sizeof(command) - 1)
        || !serial.drain())
        return {false, {}, "could not send STATUS on " + controlPort};
    std::string response;
    std::array<std::uint8_t, 256> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const int count = serial.read(bytes.data(), static_cast<int>(bytes.size()));
        if (count < 0) return {false, {}, "control port read failed on " + controlPort};
        if (count == 0) continue;
        response.append(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::size_t>(count));
        const auto newline = response.find('\n');
        if (newline != std::string::npos) {
            response.resize(newline);
            if (!response.empty() && response.back() == '\r') response.pop_back();
            if (response.rfind("STATUS ", 0) == 0) return {true, response, {}};
            return {false, response, "unexpected coprocessor STATUS reply: " + response};
        }
        if (response.size() > 2048) return {false, {}, "coprocessor STATUS reply too long"};
    }
    return {false, response, "timed out waiting for coprocessor STATUS on " + controlPort};
}

FlashResult FlashFirmware(const FlashOptions& options, FlashCallback callback) {
    std::error_code ec;
    if (!fs::is_regular_file(options.firmware, ec))
        return {false, "firmware file not found: " + options.firmware.string()};
    const fs::path programmer = options.programmer.empty()
        ? FindStm32Programmer() : options.programmer;
    if (programmer.empty() || !fs::is_regular_file(programmer, ec))
        return {false, "STM32_Programmer_CLI was not found"};

    const std::string ext = Lower(options.firmware.extension().string());
    if (ext != ".elf" && ext != ".hex" && ext != ".bin")
        return {false, "firmware must be .elf, .hex, or .bin"};
    if (options.target == FlashTarget::Coprocessor) {
        if (ext == ".bin" && fs::file_size(options.firmware, ec) > 256U * 1024U)
            return {false, "coprocessor .bin exceeds 256 KiB flash"};
        Notify(callback, FlashPhase::EnterBootloader,
               "Coprocessor must already be in STM32 USB DFU mode");
        std::vector<std::string> args = {"-c", "port=usb1", "-d", options.firmware.string()};
        if (ext == ".bin") args.emplace_back("0x08000000");
        args.emplace_back("-v");
        auto result = RunProgrammer(programmer, args, callback);
        if (!result.success) return result;
        if (const auto entry = FirmwareEntry(options.firmware)) {
            Notify(callback, FlashPhase::ExitBootloader, "Starting coprocessor application");
            result = RunProgrammer(programmer, {"-c", "port=usb1", "-g", HexAddress(*entry)}, callback);
            if (!result.success) return result;
        }
        Notify(callback, FlashPhase::Complete, "Coprocessor firmware flash complete", 100);
        return {true, {}};
    }

    const Gen7BridgePorts discovered = DiscoverGen7BridgePorts(options.serialPort);
    const std::string bridge = options.serialPort.empty() ? discovered.bridge : options.serialPort;
    const std::string control = options.controlPort.empty() ? discovered.control : options.controlPort;
    if (bridge.empty()) return {false, "Gen7 UART bridge if00 not found; pass --serial PORT"};
    if (options.automaticBoot && control.empty())
        return {false, "Gen7 control port if02 not found; pass --control-port PORT"};

    if (options.automaticBoot) {
        Notify(callback, FlashPhase::EnterBootloader,
               "Requesting BOOTLOADER on coprocessor control port " + control);
        const auto entered = ControlCommand(control, "BOOTLOADER");
        if (!entered.success) {
            const auto restored = ControlCommand(control, "APP");
            return {false, entered.error + (restored.success ? "" : "; APP restore failed: " + restored.error)};
        }
    } else {
        Notify(callback, FlashPhase::EnterBootloader,
               "Main MCU must already be in ROM bootloader mode");
    }

    FlashResult result{false, "programmer did not run"};
    const unsigned attempts = std::clamp(options.attempts, 1U, 10U);
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        std::vector<std::string> args = {"-c", "port=" + bridge, "br=460800",
            "P=EVEN", "db=8", "sb=1", "-d", options.firmware.string()};
        if (ext == ".bin") args.emplace_back("0x08000000");
        args.emplace_back("-v");
        Notify(callback, FlashPhase::Program,
               "Programming main MCU, attempt " + std::to_string(attempt)
               + "/" + std::to_string(attempts));
        result = RunProgrammer(programmer, args, callback);
        if (result.success) break;
        if (attempt < attempts && options.automaticBoot) {
            const auto reentered = ControlCommand(control, "BOOTLOADER");
            if (!reentered.success) {
                result.error += "; retry bootloader entry failed: " + reentered.error;
                break;
            }
        }
    }
    if (options.automaticBoot) {
        Notify(callback, FlashPhase::ExitBootloader,
               "Requesting APP on coprocessor control port " + control);
        const auto app = ControlCommand(control, "APP");
        if (!app.success) {
            if (!result.success) result.error += "; APP restore failed: " + app.error;
            else result = {false, "firmware verified, but APP restore failed: " + app.error};
        }
    }
    if (!result.success) return result;
    Notify(callback, FlashPhase::Complete, "Main MCU firmware flash complete", 100);
    return {true, {}};
}

}  // namespace RTEAutomation
