#include "Builder.h"
#include "Emitter.h"
#include "TraceCli.h"

#include <RTEAutomation/CachePaths.h>
#include <RTEAutomation/Flasher.h>
#include <RTEAutomation/Mcp2221Gpio.h>
#include <RTEAutomation/Platform.h>
#include <RTEAutomation/ProcessRunner.h>
#include <RTEAutomation/Session.h>

#include <NodeAPI/NodeTemplates.h>
#include <NodeAPI/Serialization.h>
#include <NodeAPI/Timing.h>
#include <RTELogger/Logger.h>
#include <inverter_protocol/host/uart_transport.h>
#include <inverter_protocol/packet_parser.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

enum class Format { Text, Json, JsonLines };
std::vector<json> jsonEvents;
std::optional<Format> commandFormatOverride;

struct Parsed {
    std::string command;
    Format format = Format::Text;
    std::vector<std::string> args;
};

void Usage() {
    std::cerr
        << "usage: rte [--format text|json|jsonl] <command> [options]\n"
        << "  validate --graph FILE [--templates DIR]\n"
        << "  generate --graph FILE --base-source DIR --output DIR [--templates DIR]\n"
        << "  build --graph FILE --base-source DIR [--source-output DIR] [--build-dir DIR]\n"
        << "        [--build-type TYPE] [--toolchain MODE] [--generator NAME] [--clean]\n"
        << "  flash --firmware FILE [--target main|coproc] [--serial PORT] [--control-port PORT]\n"
        << "        [--session FILE] [--programmer FILE] [--attempts N] [--manual-boot]\n"
        << "  mcp2221 enter|exit|release\n"
        << "  device status|telemetry|signal|history|histories|signals|snapshot|build-info|control-status|console|command|mode|ports [--session FILE]\n"
        << "  tool MCP_TOOL [--arguments JSON] [--workspace DIR] [--session FILE]\n"
        << "  device mode [--serial PORT] [--control-port PORT] [--probe-bootloader]\n"
        << "  sim --graph FILE [--scenario FILE] [--base-source DIR] [--name NAME]\n"
        << "      [--live] [--realtime F] [--no-build] [--output-format text|json|jsonl]\n"
        << "      WARNING: simulation is not correctly implemented; use is not advised\n"
        << "  trace record --interface can0 --output FILE [--seconds N] [--id-base ID]\n"
        << "  trace export --input FILE --output CSV\n"
        << "  mcp [--workspace PATH] [--session FILE]\n";
}

std::optional<Parsed> ParseTopLevel(int argc, char* argv[]) {
    Parsed parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string value = argv[i];
        if (parsed.command.empty() && value == "--format") {
            if (++i >= argc) return std::nullopt;
            const std::string format = argv[i];
            if (format == "text") parsed.format = Format::Text;
            else if (format == "json") parsed.format = Format::Json;
            else if (format == "jsonl") parsed.format = Format::JsonLines;
            else return std::nullopt;
        } else if (parsed.command.empty() && value == "--version") {
            parsed.command = "version";
        } else if (parsed.command.empty()) {
            parsed.command = value;
        } else {
            parsed.args.push_back(value);
        }
    }
    if (parsed.command.empty()) return std::nullopt;
    return parsed;
}

struct Options {
    std::optional<fs::path> graph;
    std::optional<fs::path> templates;
    std::optional<fs::path> baseSource;
    std::optional<fs::path> output;
    std::optional<fs::path> sourceOutput;
    std::optional<fs::path> buildDir;
    std::optional<fs::path> firmware;
    std::optional<fs::path> programmer;
    std::optional<fs::path> session;
    std::string serial;
    std::string controlPort;
    std::string target = "main";
    unsigned attempts = 3;
    std::string buildType = "Release";
    std::string toolchain = "auto";
    std::string generator = "Ninja";
    bool clean = false;
    bool automaticBoot = true;
};

bool ParseOptions(const std::vector<std::string>& args, Options& options,
                  std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto nextPath = [&](const char* name, std::optional<fs::path>& target) {
            if (i + 1 >= args.size()) { error = std::string("missing value for ") + name; return false; }
            target = fs::path(args[++i]);
            return true;
        };
        auto nextString = [&](const char* name, std::string& target) {
            if (i + 1 >= args.size()) { error = std::string("missing value for ") + name; return false; }
            target = args[++i];
            return true;
        };
        if (arg == "--graph") { if (!nextPath("--graph", options.graph)) return false; }
        else if (arg == "--templates") { if (!nextPath("--templates", options.templates)) return false; }
        else if (arg == "--base-source") { if (!nextPath("--base-source", options.baseSource)) return false; }
        else if (arg == "--output") { if (!nextPath("--output", options.output)) return false; }
        else if (arg == "--source-output") { if (!nextPath("--source-output", options.sourceOutput)) return false; }
        else if (arg == "--build-dir") { if (!nextPath("--build-dir", options.buildDir)) return false; }
        else if (arg == "--firmware") { if (!nextPath("--firmware", options.firmware)) return false; }
        else if (arg == "--programmer") { if (!nextPath("--programmer", options.programmer)) return false; }
        else if (arg == "--session") { if (!nextPath("--session", options.session)) return false; }
        else if (arg == "--serial") { if (!nextString("--serial", options.serial)) return false; }
        else if (arg == "--control-port") { if (!nextString("--control-port", options.controlPort)) return false; }
        else if (arg == "--target") { if (!nextString("--target", options.target)) return false; }
        else if (arg == "--attempts") {
            if (i + 1 >= args.size()) { error = "missing value for --attempts"; return false; }
            try {
                const std::string value = args[++i];
                const unsigned long parsed = std::stoul(value);
                if (parsed < 1 || parsed > 10 || std::to_string(parsed) != value) throw std::invalid_argument("range");
                options.attempts = static_cast<unsigned>(parsed);
            } catch (...) { error = "--attempts must be 1 through 10"; return false; }
        }
        else if (arg == "--build-type") { if (!nextString("--build-type", options.buildType)) return false; }
        else if (arg == "--toolchain") { if (!nextString("--toolchain", options.toolchain)) return false; }
        else if (arg == "--generator") { if (!nextString("--generator", options.generator)) return false; }
        else if (arg == "--clean") options.clean = true;
        else if (arg == "--manual-boot") options.automaticBoot = false;
        else { error = "unknown option: " + arg; return false; }
    }

    /* Resolve any relative paths to absolute against the process's starting cwd.
     * This keeps later code (emitter, builder, subprocess runners) from
     * interpreting them relative to a changed working directory. */
    auto normalize = [](std::optional<fs::path>& p) {
        if (!p || p->is_absolute()) return;
        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::absolute(*p, ec), ec);
        if (ec || abs.empty()) abs = fs::absolute(*p);
        p = std::move(abs);
    };
    normalize(options.graph);
    normalize(options.templates);
    normalize(options.baseSource);
    normalize(options.output);
    normalize(options.sourceOutput);
    normalize(options.buildDir);
    normalize(options.firmware);
    normalize(options.programmer);
    normalize(options.session);

    return true;
}

void Emit(Format format, const json& value) {
    if (format == Format::Text) {
        const std::string event = value.value("event", "");
        if (event == "error") std::cerr << "rte: " << value.value("message", "operation failed") << '\n';
        else if (event == "artifact") std::cout << value.value("path", "") << '\n';
        else if (event == "complete") std::cout << (value.value("success", false) ? "complete" : "failed") << '\n';
        else if (value.contains("message")) std::cout << value["message"].get<std::string>() << '\n';
        return;
    }
    if (format == Format::Json) {
        jsonEvents.push_back(value);
    } else {
        std::cout << value.dump() << '\n';
    }
}

bool ReadGraph(const Options& options, NodeAPI::Graph& graph,
               std::vector<std::string>& warnings, std::string& error) {
    if (!options.graph) { error = "--graph is required"; return false; }
    if (options.templates) {
        const auto loaded = NodeAPI::LoadNodeTypesFromDirectory(graph, *options.templates);
        warnings.insert(warnings.end(), loaded.warnings.begin(), loaded.warnings.end());
        if (!loaded.ok) {
            std::ostringstream text;
            text << "template loading failed";
            for (const auto& item : loaded.errors) text << "; " << item;
            error = text.str();
            return false;
        }
    }
    std::ifstream file(*options.graph);
    if (!file) { error = "could not open graph: " + options.graph->string(); return false; }
    std::ostringstream contents;
    contents << file.rdbuf();
    const auto schema = NodeAPI::CheckGraphSchema(contents.str());
    if (!schema.warning.empty()) warnings.push_back(schema.warning);
    try { NodeAPI::LoadIntoGraph(graph, contents.str()); }
    catch (const std::exception& exception) {
        error = std::string("could not parse graph: ") + exception.what();
        return false;
    }
    return true;
}

int Validate(const Options& options, Format format) {
    NodeAPI::Graph graph;
    std::vector<std::string> warnings;
    std::string error;
    if (!ReadGraph(options, graph, warnings, error)) {
        Emit(format, {{"event","error"},{"message",error}});
        return 2;
    }
    const auto timing = NodeAPI::Timing::Validator{}.Validate(graph);
    if (!timing.ok) {
        Emit(format, {{"event","error"},{"message","graph validation failed"},
                      {"errors",timing.errors},{"warnings",warnings}});
        return 2;
    }
    Emit(format, {{"event","complete"},{"success",true},
                  {"nodes",graph.GetNodes().size()},
                  {"connections",graph.GetConnections().size()},
                  {"warnings",warnings}});
    return 0;
}

int Generate(const Options& options, Format format) {
    if (!options.graph || !options.baseSource || !options.output) {
        Emit(format, {{"event","error"},{"message","--graph, --base-source, and --output are required"}});
        return 2;
    }
    Emit(format, {{"event","progress"},{"phase","generate"},{"percent",0},
                  {"message","Generating firmware source"}});
    RTECodeEmitter::Logger logger(format == Format::Text
        ? RTECodeEmitter::LogLevel::Info : RTECodeEmitter::LogLevel::Warning);
    RTECodeEmitter::EmitterOptions emitter;
    emitter.graphPath = *options.graph;
    emitter.baseSrc = *options.baseSource;
    emitter.outputDir = *options.output;
    if (options.templates) emitter.templatesDir = *options.templates;
    if (!RTECodeEmitter::Emitter(logger).Run(emitter)) {
        Emit(format, {{"event","error"},{"message","firmware generation failed"}});
        return 4;
    }
    Emit(format, {{"event","artifact"},{"kind","generated-source"},
                  {"path",fs::absolute(*options.output).string()}});
    Emit(format, {{"event","complete"},{"success",true}});
    return 0;
}

int Build(Options options, Format format) {
    if (!options.graph || !options.baseSource) {
        Emit(format, {{"event","error"},{"message","--graph and --base-source are required"}});
        return 2;
    }
    auto workspace = RTEAutomation::WorkspaceForGraph(*options.graph, options.buildType);
    if (!options.sourceOutput) options.sourceOutput = workspace.generated;
    if (!options.buildDir) options.buildDir = workspace.build;
    std::error_code ec;
    fs::create_directories(workspace.artifacts, ec);

    Emit(format, {{"event","progress"},{"phase","build"},{"percent",0},
                  {"message","Building firmware"},
                  {"workspace",workspace.root.string()}});
    RTECodeEmitter::Logger logger(format == Format::Text
        ? RTECodeEmitter::LogLevel::Info : RTECodeEmitter::LogLevel::Warning);
    RTEFirmwareBuilder::BuilderOptions builder;
    builder.fwSrc = *options.baseSource;
    builder.buildDir = *options.buildDir;
    builder.graphPath = *options.graph;
    builder.baseSrc = *options.baseSource;
    builder.outputDir = *options.sourceOutput;
    builder.templatesDir = options.templates;
    builder.buildType = options.buildType;
    builder.toolchainMode = options.toolchain;
    builder.generator = options.generator;
    builder.clean = options.clean;
    if (!RTEFirmwareBuilder::Builder(logger).Run(builder)) {
        Emit(format, {{"event","error"},{"message","firmware build failed"},
                      {"workspace",workspace.root.string()}});
        return 4;
    }
    const fs::path elf = *options.buildDir / "STM32CubeMX.elf";
    const fs::path binary = *options.buildDir / "STM32CubeMX.bin";
    const fs::path artifactElf = workspace.artifacts / elf.filename();
    const fs::path artifactBinary = workspace.artifacts / binary.filename();
    fs::create_directories(workspace.artifacts, ec);
    if (ec || !fs::is_regular_file(elf) || !fs::is_regular_file(binary)) {
        Emit(format, {{"event","error"},{"message","firmware build did not produce expected artifacts"}});
        return 4;
    }
    fs::copy_file(elf, artifactElf, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::copy_file(binary, artifactBinary,
                           fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Emit(format, {{"event","error"},{"message","could not stage firmware artifacts: " + ec.message()}});
        return 4;
    }
    json manifest = {{"graph",fs::absolute(*options.graph).string()},
                     {"configuration",options.buildType},
                     {"generated",fs::absolute(*options.sourceOutput).string()},
                     {"build",fs::absolute(*options.buildDir).string()},
                     {"firmware",fs::absolute(artifactBinary).string()}};
    std::ofstream(workspace.manifest) << manifest.dump(2) << '\n';
    Emit(format, {{"event","artifact"},{"kind","firmware-bin"},
                  {"path",fs::absolute(artifactBinary).string()}});
    Emit(format, {{"event","artifact"},{"kind","firmware-elf"},
                  {"path",fs::absolute(artifactElf).string()}});
    Emit(format, {{"event","complete"},{"success",true},
                  {"workspace",workspace.root.string()}});
    return 0;
}

int Flash(const Options& options, Format format) {
    if (!options.firmware) {
        Emit(format, {{"event","error"},{"message","--firmware is required"}});
        return 2;
    }
    if (options.target != "main" && options.target != "coproc") {
        Emit(format, {{"event","error"},{"message","--target must be main or coproc"}});
        return 2;
    }
    std::string serial = options.serial;
    std::optional<RTEAutomation::SessionDescriptor> session;
    bool studioLease = false;
    std::string sessionError;
    if (options.target == "main" && serial.empty()
        && (options.session || fs::exists(RTEAutomation::CurrentSessionPath()))) {
        session = RTEAutomation::DiscoverSession(options.session.value_or(fs::path{}), &sessionError);
        if (!session) {
            Emit(format, {{"event","error"},{"message",sessionError}});
            return 5;
        }
        const auto lease = RTEAutomation::RequestSession(
            *session, "device.flash.begin", json::object(), &sessionError);
        if (!lease) {
            Emit(format, {{"event","error"},{"message",sessionError}});
            return 5;
        }
        serial = lease->value("device_port", "");
        studioLease = true;
    }
    RTEAutomation::FlashOptions flash;
    flash.firmware = *options.firmware;
    flash.serialPort = serial;
    flash.controlPort = options.controlPort;
    flash.target = options.target == "coproc"
        ? RTEAutomation::FlashTarget::Coprocessor : RTEAutomation::FlashTarget::Main;
    flash.automaticBoot = options.automaticBoot;
    flash.attempts = options.attempts;
    if (options.programmer) flash.programmer = *options.programmer;
    const auto result = RTEAutomation::FlashFirmware(
        flash, [&](const RTEAutomation::FlashEvent& event) {
            Emit(format, {{"event","progress"},
                          {"phase",RTEAutomation::FlashPhaseName(event.phase)},
                          {"percent",event.percent},
                          {"message",event.message}});
        });
    if (studioLease) {
        std::string ignored;
        RTEAutomation::RequestSession(*session, "device.flash.end",
                                      {{"success", result.success}}, &ignored);
    }
    if (!result.success) {
        Emit(format, {{"event","error"},{"message",result.error}});
        return result.error.find("not found") != std::string::npos ? 3 : 4;
    }
    Emit(format, {{"event","complete"},{"success",true}});
    return 0;
}

int Mcp2221(const std::vector<std::string>& args, Format format) {
    if (args.size() != 1) {
        Emit(format, {{"event","error"},
                      {"message","mcp2221 requires enter, exit, or release"}});
        return 2;
    }
    RTEAutomation::Mcp2221GpioAction action;
    if (args[0] == "enter")
        action = RTEAutomation::Mcp2221GpioAction::EnterBootloader;
    else if (args[0] == "exit")
        action = RTEAutomation::Mcp2221GpioAction::StartApplication;
    else if (args[0] == "release")
        action = RTEAutomation::Mcp2221GpioAction::ReleasePins;
    else {
        Emit(format, {{"event","error"},
                      {"message","unknown mcp2221 action: " + args[0]}});
        return 2;
    }
    const auto result = RTEAutomation::ControlMcp2221Gpio(action);
    if (!result.success) {
        Emit(format, {{"event","error"},{"message",result.error}});
        return result.error.find("not found") != std::string::npos ? 3 : 4;
    }
    Emit(format, {{"event","complete"},{"success",true},
                  {"message","MCP2221A GPIO action completed"}});
    return 0;
}

void EmitDeviceResult(Format format, const std::string& operation, const json& result) {
    if (format == Format::Text) {
        std::cout << result.dump(2) << '\n';
    } else {
        Emit(format, {{"event", "device"}, {"operation", operation}, {"result", result}});
    }
}

bool SameSerialPort(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) return false;
    if (left == right) return true;
    std::error_code ec;
    return fs::equivalent(left, right, ec) && !ec;
}

unsigned CountAppFrames(const std::string& port, int sampleMs, std::string& error) {
    ivp::UartTransport transport;
    if (!transport.open(port, 460800)) {
        error = "could not open UART bridge " + port;
        return 0;
    }
    unsigned valid = 0;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(sampleMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<std::uint8_t, ivp::UartTransport::RX_FRAME_CAP> packet{};
        const int length = transport.receivePacket(packet.data(), packet.size());
        if (length > 0) {
            ivp_header_t header{};
            const std::uint8_t* payload = nullptr;
            std::uint16_t payloadLength = 0;
            if (ivp_packet_parse(packet.data(), static_cast<std::size_t>(length),
                                 &header, &payload, &payloadLength) == IVP_OK) ++valid;
        }
        if (valid >= 2) break;
        if (length <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return valid;
}

json ProbeMainMode(const std::string& preferredBridge,
                   const std::string& preferredControl,
                   const fs::path& sessionPath,
                   bool probeBootloader,
                   const fs::path& preferredProgrammer) {
    const auto discovered = RTEAutomation::DiscoverGen7BridgePorts(preferredBridge);
    const std::string bridge = preferredBridge.empty() ? discovered.bridge : preferredBridge;
    const std::string control = preferredControl.empty() ? discovered.control : preferredControl;
    json report = {{"state", "unknown"}, {"bridge_port", bridge},
                   {"control_port", control}, {"app_frames_observed", 0},
                   {"notes", json::array()}};
    const auto controlStatus = RTEAutomation::ReadGen7ControlStatus(control);
    if (controlStatus.success) {
        report["control_status"] = controlStatus.line;
        if (controlStatus.line.find("UART_MODE=BOOT_8E1") != std::string::npos)
            report["bridge_mode"] = "bootloader";
        else if (controlStatus.line.find("UART_MODE=APP_8N1") != std::string::npos)
            report["bridge_mode"] = "app";
        else report["bridge_mode"] = "unknown";
    } else {
        report["bridge_mode"] = "unknown";
        report["notes"].push_back(controlStatus.error);
    }

    std::string sessionError;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &sessionError);
    bool studioOwnsBridge = false;
    if (session) {
        const auto status = RTEAutomation::RequestSession(*session, "device.status",
                                                           json::object(), &sessionError);
        if (status && status->value("transport", "unknown") == "serial"
            && SameSerialPort(status->value("device_port", ""), bridge)
            && !status->value("suspended", false)) {
            studioOwnsBridge = true;
            report["observation_source"] = "RTE Studio telemetry";
        }
    }

    if (report["bridge_mode"] == "bootloader") {
        report["state"] = "bootloader_selected";
        report["notes"].push_back("The coprocessor selected bootloader UART and reset the main MCU into boot mode; this alone does not prove the ROM bootloader responds.");
        if (probeBootloader && !bridge.empty() && !studioOwnsBridge) {
            const fs::path programmer = preferredProgrammer.empty()
                ? RTEAutomation::FindStm32Programmer() : preferredProgrammer;
            if (programmer.empty()) {
                report["notes"].push_back("STM32_Programmer_CLI not found; bootloader handshake was not checked.");
            } else {
                RTEAutomation::ProcessSpec process;
                process.executable = programmer;
                process.arguments = {"-c", "port=" + bridge, "br=460800", "P=EVEN", "db=8", "sb=1"};
                const auto probe = RTEAutomation::RunProcess(process, [](const std::string&) {});
                report["bootloader_probe_success"] = probe.started && probe.exitCode == 0;
                report["state"] = probe.started && probe.exitCode == 0
                    ? "bootloader_responding" : "bootloader_unresponsive";
                if (!probe.started) report["notes"].push_back(probe.error);
            }
        } else if (probeBootloader && studioOwnsBridge) {
            report["notes"].push_back("Bootloader probe skipped because RTE Studio owns the UART bridge.");
        }
        return report;
    }

    unsigned frames = 0;
    if (studioOwnsBridge) {
        const auto first = RTEAutomation::RequestSession(*session, "device.telemetry",
                                                          json::object(), &sessionError);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const auto second = RTEAutomation::RequestSession(*session, "device.telemetry",
                                                           json::object(), &sessionError);
        if (first && second) {
            const auto before = first->value("good_frames", std::uint64_t{0});
            const auto after = second->value("good_frames", std::uint64_t{0});
            frames = static_cast<unsigned>(after > before ? after - before : 0);
        } else report["notes"].push_back(sessionError);
    } else if (!bridge.empty()) {
        std::string probeError;
        frames = CountAppFrames(bridge, 750, probeError);
        report["observation_source"] = "passive UART frame sample";
        if (!probeError.empty()) report["notes"].push_back(probeError);
    } else report["notes"].push_back("UART bridge not found; pass --serial PORT.");
    report["app_frames_observed"] = frames;
    if (frames > 0) {
        report["state"] = "app_responding";
    } else if (report["bridge_mode"] == "app") {
        report["state"] = "app_unresponsive_or_silent";
        report["notes"].push_back("No valid app frames were observed. The main MCU may be hung, silent, unpowered, or disconnected; this cannot be distinguished without a main MCU heartbeat or external health signal.");
    }
    return report;
}

int DeviceMode(const std::vector<std::string>& args, Format format) {
    std::string serial, control;
    fs::path session, programmer;
    bool probeBootloader = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--probe-bootloader") { probeBootloader = true; continue; }
        if (args[i] != "--serial" && args[i] != "--control-port"
            && args[i] != "--session" && args[i] != "--programmer") {
            Emit(format, {{"event", "error"}, {"message", "unknown device mode option: " + args[i]}});
            return 2;
        }
        if (i + 1 >= args.size()) {
            Emit(format, {{"event", "error"}, {"message", "missing value for " + args[i]}});
            return 2;
        }
        if (args[i] == "--serial") serial = args[++i];
        else if (args[i] == "--control-port") control = args[++i];
        else if (args[i] == "--session") session = args[++i];
        else if (args[i] == "--programmer") programmer = args[++i];
    }
    EmitDeviceResult(format, "mode", ProbeMainMode(serial, control, session,
                                                    probeBootloader, programmer));
    return 0;
}

int Device(const std::vector<std::string>& args, Format format) {
    if (args.empty()) {
        Emit(format, {{"event", "error"}, {"message", "device subcommand is required"}});
        return 2;
    }
    const std::string operation = args.front();
    if (operation == "mode") return DeviceMode(
        std::vector<std::string>(args.begin() + 1, args.end()), format);
    if (operation == "ports") {
        const auto ports = RTEAutomation::DiscoverGen7BridgePorts();
        EmitDeviceResult(format, operation, {{"bridge", ports.bridge}, {"control", ports.control}});
        return 0;
    }
    fs::path sessionPath;
    std::uint64_t since = 0;
    std::size_t lines = 100;
    std::size_t limit = 1000;
    double window = 5.0;
    std::vector<std::string> signals;
    std::string filter;
    std::string bundle = "foc";
    std::string command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--session" && i + 1 < args.size()) sessionPath = args[++i];
        else if (args[i] == "--since" && i + 1 < args.size()) {
            try { since = std::stoull(args[++i]); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --since value"}}); return 2; }
        } else if (args[i] == "--lines" && i + 1 < args.size()) {
            try { lines = static_cast<std::size_t>(std::stoull(args[++i])); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --lines value"}}); return 2; }
        } else if (args[i] == "--limit" && i + 1 < args.size()) {
            try { limit = static_cast<std::size_t>(std::stoull(args[++i])); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --limit value"}}); return 2; }
        } else if (args[i] == "--window-s" && i + 1 < args.size()) {
            try { window = std::stod(args[++i]); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --window-s value"}}); return 2; }
        } else if (args[i] == "--signal" && i + 1 < args.size()) signals.push_back(args[++i]);
        else if (args[i] == "--filter" && i + 1 < args.size()) filter = args[++i];
        else if (args[i] == "--bundle" && i + 1 < args.size()) bundle = args[++i];
        else if (args[i] == "--command" && i + 1 < args.size()) command = args[++i];
        else if (operation == "command") {
            if (!command.empty()) command += ' ';
            command += args[i];
        } else {
            Emit(format, {{"event","error"},{"message","unknown device option: " + args[i]}});
            return 2;
        }
    }
    std::string error;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &error);
    if (!session) {
        Emit(format, {{"event","error"},{"message",error}});
        return 5;
    }
    std::string method;
    json params = json::object();
    if (operation == "status") method = "device.status";
    else if (operation == "telemetry") method = "device.telemetry";
    else if (operation == "build-info") method = "device.build_info";
    else if (operation == "control-status") method = "device.control_status";
    else if (operation == "snapshot") {
        method = "device.snapshot";
        params = {{"bundle", bundle}, {"signals", signals}};
    }
    else if (operation == "signals") {
        method = "device.catalog";
        params = {{"filter", filter}, {"signal", signals.empty() ? "" : signals.front()}};
    } else if (operation == "signal" || operation == "history"
               || operation == "string-history") {
        if (signals.size() != 1) {
            Emit(format, {{"event", "error"}, {"message", "exactly one --signal is required"}});
            return 2;
        }
        method = operation == "string-history" ? "device.string_history"
               : operation == "history" ? "device.history" : "device.telemetry";
        params = {{"signal", signals.front()}, {"limit", limit}};
    } else if (operation == "histories") {
        if (signals.empty() || signals.size() > 8 || !std::isfinite(window)
            || window < 0.05 || window > 30.0) {
            Emit(format, {{"event", "error"},
                          {"message", "use 1 to 8 --signal names and --window-s 0.05..30"}});
            return 2;
        }
        method = "device.histories";
        params = {{"signals", signals}, {"limit", limit}, {"window_s", window}};
    }
    else if (operation == "console") {
        method = "device.console";
        params = {{"since", since}, {"lines", lines}};
    } else if (operation == "command") {
        if (command.empty()) {
            Emit(format, {{"event","error"},{"message","device command text is required"}});
            return 2;
        }
        method = "device.command";
        params = {{"command", command}, {"source", "cli"}};
    } else {
        Emit(format, {{"event","error"},{"message","unknown device subcommand: " + operation}});
        return 2;
    }
    const auto result = RTEAutomation::RequestSession(*session, method, params, &error);
    if (!result) {
        Emit(format, {{"event","error"},{"message",error}});
        return 5;
    }
    if (operation == "signal") {
        const auto& name = signals.front();
        const auto& numbers = result->at("signals");
        const auto& strings = result->at("strings");
        const auto status = result->value("signal_status", json::object());
        const auto state = status.value(name, json::object());
        const auto lastKnown = result->value("last_known_values", json::object());
        if (numbers.contains(name)) EmitDeviceResult(format, operation,
            {{"signal", name}, {"kind", "number"}, {"value", numbers.at(name)},
             {"last_value", lastKnown.value(name, numbers.at(name))}, {"status", state}});
        else if (strings.contains(name)) EmitDeviceResult(format, operation,
            {{"signal", name}, {"kind", "string"}, {"value", strings.at(name)},
             {"last_value", lastKnown.value(name, strings.at(name))}, {"status", state}});
        else {
            Emit(format, {{"event", "error"}, {"message", "unknown signal: " + name}});
            return 5;
        }
        return 0;
    }
    EmitDeviceResult(format, operation, *result);
    return 0;
}

json McpText(const std::string& text, bool error = false) {
    json result = {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
    if (error) result["isError"] = true;
    return result;
}

json McpJson(const json& value) {
    return {{"content", json::array({{{"type", "text"}, {"text", value.dump(2)}}})},
            {"structuredContent", value}};
}

json ToolDefinition(const std::string& name, const std::string& description,
                    json properties, std::vector<std::string> required = {}) {
    // An empty braced initializer would construct a null JSON value, which
    // violates the MCP tool schema ("properties" must be an object).
    if (properties.is_null()) properties = json::object();
    json schema = {{"type", "object"}, {"properties", std::move(properties)},
                   {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = std::move(required);
    const bool readOnly = name == "rte_project_info" || name == "rte_graph_read"
        || name == "rte_validate" || name == "rte_bridge_ports"
        || name == "rte_device_status" || name == "rte_device_telemetry"
        || name == "rte_device_mode"
        || name == "rte_device_signal" || name == "rte_device_history"
        || name == "rte_device_string_history"
        || name == "rte_device_console" || name == "rte_build_info"
        || name == "rte_signal_info" || name == "rte_control_status"
        || name == "rte_device_histories" || name == "rte_device_trends"
        || name == "rte_device_snapshot" || name == "rte_device_commands";
    const bool destructive = name == "rte_flash" || name == "rte_device_command"
        || name == "rte_spike_capture"
        || name == "rte_device_command_response" || name == "rte_mcp2221";
    return {{"name", name}, {"description", description},
            {"inputSchema", std::move(schema)},
            {"annotations", {{"readOnlyHint", readOnly}, {"destructiveHint", destructive}}}};
}

json McpTools() {
    const json path = {{"type", "string"}};
    return json::array({
        ToolDefinition("rte_project_info", "List the RTE workspace and available graph files.", {}),
        ToolDefinition("rte_graph_read", "Read a graph JSON file from the workspace.",
            {{"graph", path}}, {"graph"}),
        ToolDefinition("rte_validate", "Validate an RTE graph without changing files.",
            {{"graph", path}, {"templates", path}}, {"graph"}),
        ToolDefinition("rte_generate", "Generate firmware sources from an RTE graph.",
            {{"graph", path}, {"base_source", path}, {"output", path}, {"templates", path}},
            {"graph", "base_source", "output"}),
        ToolDefinition("rte_build", "Generate and build firmware in the RTE user cache.",
            {{"graph", path}, {"base_source", path}, {"templates", path},
             {"build_type", {{"type", "string"}}}}, {"graph", "base_source"}),
        ToolDefinition("rte_sim", "UNRELIABLE: simulation is not correctly implemented and is not advised for control or hardware decisions. Run only for simulator development.",
            {{"graph", path}, {"scenario", path}, {"base_source", path},
             {"name", {{"type", "string"}}}, {"no_build", {{"type", "boolean"}}}}, {"graph"}),
        ToolDefinition("rte_trace_export", "Export a recorded RTE CAN trace to CSV.",
            {{"input", path}, {"output", path}}, {"input", "output"}),
        ToolDefinition("rte_trace_record", "Record a bounded CAN trace to a capture file.",
            {{"interface", {{"type", "string"}}}, {"output", path},
             {"seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
             {"id_base", {{"type", "string"}}}}, {"interface", "output", "seconds"}),
        ToolDefinition("rte_bridge_ports", "Discover the Gen7 UART bridge and coprocessor control ports.", {}),
        ToolDefinition("rte_mcp2221", "Legacy Gen6 MCP2221A boot/reset GPIO action.",
            {{"action", {{"type", "string"}, {"enum", json::array({"enter", "exit", "release"})}}}},
            {"action"}),
        ToolDefinition("rte_flash", "Flash main MCU over the Gen7 UART bridge or coprocessor over USB DFU. Returns one compact success result or a bounded error message; programmer progress is kept out of the agent response.",
            {{"firmware", path}, {"serial", {{"type", "string"}}},
             {"control_port", {{"type", "string"}}},
             {"target", {{"type", "string"}, {"enum", json::array({"main", "coproc"})}}},
             {"programmer", path},
             {"attempts", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}},
             {"manual_boot", {{"type", "boolean"}}}}, {"firmware"}),
        ToolDefinition("rte_device_status", "Read the active RTE Studio device status.", {}),
        ToolDefinition("rte_device_mode", "Diagnose Gen7 main MCU app/bootloader responsiveness without changing firmware. Silent app state is inconclusive.",
            {{"serial", {{"type", "string"}}}, {"control_port", {{"type", "string"}}},
             {"probe_bootloader", {{"type", "boolean"}}}, {"programmer", path}}),
        ToolDefinition("rte_device_telemetry", "Read current telemetry and per-signal reporting state. Stopped signals have null current values and separate last-known values.", {}),
        ToolDefinition("rte_build_info", "Read the graph identity and node/signal manifest announced by the running firmware.", {}),
        ToolDefinition("rte_signal_info", "List observed and firmware-declared signals, their source nodes, units, and freshness.",
            {{"filter", {{"type", "string"}}}, {"signal", {{"type", "string"}}}}),
        ToolDefinition("rte_control_status", "Read the main RTE graph motor control state, latched fault names, and actual PWM/gate status.", {}),
        ToolDefinition("rte_device_snapshot", "Read a named FOC bundle or up to 32 selected signals from one store snapshot, with freshness and missing keys.",
            {{"bundle", {{"type", "string"}, {"enum", json::array({"foc"})}}},
             {"signals", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}, {"maxItems", 32}}}}),
        ToolDefinition("rte_device_signal", "Read one telemetry signal. When reporting stops, value is null and last_value retains the old measurement.",
            {{"signal", {{"type", "string"}}}}, {"signal"}),
        ToolDefinition("rte_device_history", "Read recent time and value samples for one numeric telemetry signal.",
            {{"signal", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 12000}}}}, {"signal"}),
        ToolDefinition("rte_device_string_history", "Read recent string telemetry events for one signal.",
            {{"signal", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}, {"signal"}),
        ToolDefinition("rte_device_histories", "Read up to eight numeric histories with individual timestamps and reporting state. The live window advances even after samples stop.",
            {{"signals", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}, {"maxItems", 8}}},
             {"window_s", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 30.0}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4000}}}}, {"signals"}),
        ToolDefinition("rte_device_trends", "Analyze live telemetry time series and show stopped-reporting states: sparklines, slopes, changes, spikes, and resolved oscillation. Defaults to available FOC signals.",
            {{"signals", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}, {"maxItems", 8}}},
             {"window_s", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 30.0}}}}),
        ToolDefinition("rte_spike_capture", "Dump and re-arm the firmware's frozen 64-sample, 5 kHz current-spike capture. Returns parsed samples and current/angle trend charts; sends the spikes inverter command.",
            {{"timeout_ms", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 10000}}}}),
        ToolDefinition("rte_device_console", "Read device console lines from RTE Studio.",
            {{"since", {{"type", "integer"}, {"minimum", 0}}},
             {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}),
        ToolDefinition("rte_device_commands", "Discover every command registered by the connected firmware by sending help; returns names, usage, descriptions, argument ranges, and motor control command guidance. Use control for the main RTE graph motor control; foc is the internal test native diagnostic. Requires external command writes enabled in Studio.",
            {{"timeout_ms", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 10000}}}}),
        ToolDefinition("rte_device_command",
            "Send any inverter command through RTE Studio. Returns a console cursor for reading its reply. Requires external writes enabled in Studio.",
            {{"command", {{"type", "string"}}}}, {"command"}),
        ToolDefinition("rte_device_command_response",
            "Send an inverter command and collect console lines received afterward. Replies are time-window observations and may include unrelated device output.",
            {{"command", {{"type", "string"}}},
             {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}, {"command"})
    });
}

json RunCliTool(const std::string& tool, const json& arguments,
                const fs::path& workspace, const fs::path& sessionPath) {
    RTEAutomation::ProcessSpec process;
    process.executable = RTEAutomation::ExecutablePath();
    process.workingDirectory = workspace;
    process.arguments = {"--format", "jsonl"};
    auto addPath = [&](const char* option, const char* key) {
        if (arguments.contains(key)) {
            process.arguments.emplace_back(option);
            process.arguments.push_back(arguments[key].get<std::string>());
        }
    };
    if (tool == "rte_validate") {
        process.arguments.emplace_back("validate");
        addPath("--graph", "graph"); addPath("--templates", "templates");
    } else if (tool == "rte_generate") {
        process.arguments.emplace_back("generate");
        addPath("--graph", "graph"); addPath("--base-source", "base_source");
        addPath("--output", "output"); addPath("--templates", "templates");
    } else if (tool == "rte_build") {
        process.arguments.emplace_back("build");
        addPath("--graph", "graph"); addPath("--base-source", "base_source");
        addPath("--templates", "templates"); addPath("--build-type", "build_type");
    } else if (tool == "rte_flash") {
        process.arguments.emplace_back("flash");
        addPath("--firmware", "firmware"); addPath("--serial", "serial");
        if (!sessionPath.empty() && !arguments.contains("serial")) {
            process.arguments.emplace_back("--session");
            process.arguments.push_back(sessionPath.string());
        }
        addPath("--control-port", "control_port"); addPath("--target", "target");
        addPath("--programmer", "programmer");
        if (arguments.contains("attempts")) {
            process.arguments.emplace_back("--attempts");
            process.arguments.push_back(std::to_string(arguments["attempts"].get<unsigned>()));
        }
        if (arguments.value("manual_boot", false)) process.arguments.emplace_back("--manual-boot");
    } else if (tool == "rte_sim") {
        process.arguments.emplace_back("sim");
        addPath("--graph", "graph"); addPath("--scenario", "scenario");
        addPath("--base-source", "base_source"); addPath("--name", "name");
        if (arguments.value("no_build", false)) process.arguments.emplace_back("--no-build");
        process.arguments.emplace_back("--output-format");
        process.arguments.emplace_back("jsonl");
    } else if (tool == "rte_trace_export") {
        process.arguments.emplace_back("trace");
        process.arguments.emplace_back("export");
        addPath("--input", "input"); addPath("--output", "output");
    } else if (tool == "rte_trace_record") {
        process.arguments.emplace_back("trace");
        process.arguments.emplace_back("record");
        addPath("--interface", "interface"); addPath("--output", "output");
        addPath("--id-base", "id_base");
        process.arguments.emplace_back("--seconds");
        process.arguments.push_back(std::to_string(arguments["seconds"].get<unsigned>()));
    } else if (tool == "rte_mcp2221") {
        process.arguments.emplace_back("mcp2221");
        process.arguments.push_back(arguments["action"].get<std::string>());
    } else return McpText("unknown tool: " + tool, true);
    const bool compactFlash = tool == "rte_flash";
    std::string output;
    std::string flashError;
    bool flashComplete = false;
    const auto result = RTEAutomation::RunProcess(process, [&](const std::string& line) {
        if (compactFlash) {
            const json event = json::parse(line, nullptr, false);
            if (!event.is_object()) return;
            const std::string type = event.value("event", "");
            if (type == "error") flashError = event.value("message", "firmware flash failed");
            else if (type == "complete" && event.value("success", false)) flashComplete = true;
            return;
        }
        output += line;
        output += '\n';
    });
    if (compactFlash) {
        auto respond = [&](bool success, std::string message) {
            constexpr std::size_t kMaxErrorLength = 2048;
            if (message.size() > kMaxErrorLength) {
                message.resize(kMaxErrorLength);
                message += "...";
            }
            json response = McpText(success ? "Firmware flash succeeded."
                                            : "Firmware flash failed: " + message,
                                    !success);
            response["structuredContent"] = success
                ? json{{"success", true}, {"target", arguments.value("target", "main")}}
                : json{{"success", false}, {"error", message}};
            return response;
        };
        if (!result.started) return respond(false,
            result.error.empty() ? "could not start flash process" : result.error);
        if (result.exitCode != 0) return respond(false,
            flashError.empty() ? (result.error.empty() ? "operation failed" : result.error)
                               : flashError);
        if (!flashComplete) return respond(false, "flash process returned no completion result");
        return respond(true, {});
    }
    if (!result.started) return McpText(result.error, true);
    if (result.exitCode != 0)
        return McpText(output.empty() ? "operation failed" : output, true);
    json events = json::array();
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const json event = json::parse(line, nullptr, false);
        if (event.is_discarded()) return McpText(output);
        events.push_back(event);
    }
    return McpJson({{"events", std::move(events)}, {"success", true}});
}

std::optional<fs::path> WorkspaceFile(const fs::path& workspace,
                                      const std::string& requested) {
    if (requested.empty()) return std::nullopt;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(workspace, ec);
    if (ec) return std::nullopt;
    const fs::path path = fs::weakly_canonical(
        fs::path(requested).is_absolute() ? fs::path(requested) : root / requested, ec);
    if (ec) return std::nullopt;
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") return std::nullopt;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    return path;
}

json ProjectInfo(const fs::path& workspace) {
    json graphs = json::array();
    std::error_code ec;
    fs::recursive_directory_iterator it(workspace, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && graphs.size() < 200; it.increment(ec)) {
        if (it->is_directory(ec)) {
            const std::string name = it->path().filename().string();
            if (name == ".git" || name == "build" || name == "node_modules") it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".json") continue;
        if (it->path().filename() == "node.json") continue;
        const std::string relative = it->path().lexically_relative(workspace).generic_string();
        if (relative.find("graph") != std::string::npos
            || relative.find("Assets/Examples/") == 0) graphs.push_back(relative);
    }
    return {{"workspace", workspace.string()}, {"graphs", std::move(graphs)}};
}

json ValidateToolArguments(const std::string& name, const json& arguments) {
    for (const auto& definition : McpTools()) {
        if (definition["name"] != name) continue;
        if (!arguments.is_object()) return McpText("tool arguments must be an object", true);
        const auto& schema = definition["inputSchema"];
        const auto& properties = schema["properties"];
        for (const auto& key : schema.value("required", std::vector<std::string>{}))
            if (!arguments.contains(key)) return McpText("missing required argument: " + key, true);
        for (auto it = arguments.begin(); it != arguments.end(); ++it) {
            if (!properties.contains(it.key())) return McpText("unknown argument: " + it.key(), true);
            const auto& property = properties[it.key()];
            const std::string type = property.value("type", "");
            if ((type == "string" && !it.value().is_string())
                || (type == "boolean" && !it.value().is_boolean())
                || (type == "integer" && !it.value().is_number_integer())
                || (type == "number" && !it.value().is_number())
                || (type == "array" && !it.value().is_array()))
                return McpText("invalid type for argument: " + it.key(), true);
            if (type == "integer") {
                const auto number = it.value().get<std::int64_t>();
                if ((property.contains("minimum") && number < property["minimum"].get<std::int64_t>())
                    || (property.contains("maximum") && number > property["maximum"].get<std::int64_t>()))
                    return McpText("argument out of range: " + it.key(), true);
            }
            if (type == "number") {
                const double number = it.value().get<double>();
                if ((property.contains("minimum") && number < property["minimum"].get<double>())
                    || (property.contains("maximum") && number > property["maximum"].get<double>()))
                    return McpText("argument out of range: " + it.key(), true);
            }
            if (type == "array") {
                if ((property.contains("minItems") && it.value().size() < property["minItems"].get<std::size_t>())
                    || (property.contains("maxItems") && it.value().size() > property["maxItems"].get<std::size_t>()))
                    return McpText("array size out of range: " + it.key(), true);
                if (property.contains("items") && property["items"].value("type", "") == "string")
                    for (const auto& item : it.value())
                        if (!item.is_string()) return McpText("array items must be strings: " + it.key(), true);
            }
            if (property.contains("enum") && std::find(property["enum"].begin(), property["enum"].end(), it.value()) == property["enum"].end())
                return McpText("invalid value for argument: " + it.key(), true);
        }
        return json();
    }
    return McpText("unknown tool: " + name, true);
}

bool IsFrequentMcpRead(const std::string& name) {
    return name == "rte_device_status" || name == "rte_device_telemetry"
        || name == "rte_device_signal" || name == "rte_device_history"
        || name == "rte_device_string_history" || name == "rte_device_console"
        || name == "rte_device_histories" || name == "rte_device_trends"
        || name == "rte_signal_info" || name == "rte_control_status"
        || name == "rte_device_snapshot";
}

void RecordMcpActivity(const fs::path& sessionPath, const std::string& name,
                       const json& arguments, const std::string& state) {
    std::string error;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &error);
    if (!session) return;
    json visibleArguments = arguments;
    if (visibleArguments.is_object()) visibleArguments.erase("command");
    std::string detail = visibleArguments.empty() ? "" : visibleArguments.dump(-1, ' ', true);
    if (detail.size() > 480) detail = detail.substr(0, 477) + "...";
    RTEAutomation::RequestSession(*session, "automation.activity",
        {{"action", name}, {"detail", detail}, {"state", state}}, &error);
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    const double upper = *middle;
    if (values.size() % 2) return upper;
    const double lower = *std::max_element(values.begin(), middle);
    return (lower + upper) * 0.5;
}

json McpTrends(const json& histories) {
    constexpr std::size_t width = 48;
    constexpr std::array<const char*, 8> levels = {
        "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const double end = histories.value("window_end_s", 0.0);
    const double window = histories.value("window_s", 5.0);
    std::ostringstream chart;
    chart << "Recent " << window << " s; rows are auto-scaled, left older and right newer. "
          << "Analysis uses recorded sample times.\n";
    json metrics = json::object();
    const json signalStatus = histories.value("signal_status", json::object());
    for (auto it = histories.at("series").begin(); it != histories.at("series").end(); ++it) {
        const std::string name = it.key();
        const json& samples = it.value();
        const json status = signalStatus.value(name, json::object());
        const std::string reportingState = status.value("state", "unknown");
        const bool stopped = reportingState != "unknown" && reportingState != "live";
        if (samples.empty()) {
            chart << name << "  [no samples in window; " << reportingState << "]\n";
            metrics[name] = {{"samples", 0}, {"quality", "limited"},
                             {"pattern", stopped ? reportingState : "no_recent_samples"},
                             {"reporting_state", reportingState}};
            continue;
        }
        struct Point { double t, y; };
        std::vector<Point> points;
        points.reserve(samples.size());
        std::size_t invalidSamples = 0;
        std::size_t timestampResets = 0;
        for (const auto& sample : samples) {
            if (!sample.is_object() || !sample.contains("time_s")
                || !sample.contains("value") || !sample["time_s"].is_number()
                || !sample["value"].is_number()) {
                ++invalidSamples;
                continue;
            }
            const double t = sample.at("time_s").get<double>();
            const double value = sample.at("value").get<double>();
            if (!std::isfinite(t) || !std::isfinite(value)) {
                ++invalidSamples;
                continue;
            }
            // If a source clock restarts, analyze only its newest monotonic
            // segment rather than mixing samples across the discontinuity.
            if (!points.empty() && t < points.back().t) {
                points.clear();
                ++timestampResets;
            }
            points.push_back({t, value});
        }
        if (points.empty()) {
            chart << name << "  [no finite numeric samples in window]\n";
            metrics[name] = {{"samples", 0}, {"invalid_samples", invalidSamples},
                             {"timestamp_resets", timestampResets},
                             {"quality", "limited"}, {"pattern", "no_finite_samples"},
                             {"reporting_state", reportingState}};
            continue;
        }

        std::array<double, width> bucketSum{};
        std::array<double, width> bucketTimeSum{};
        std::array<double, width> bucketMin{};
        std::array<double, width> bucketMax{};
        std::array<std::size_t, width> bucketCount{};
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        double sum = 0.0, squareSum = 0.0;
        double first = 0.0, last = 0.0;
        std::size_t count = 0;
        for (const auto& point : points) {
            const double t = point.t;
            const double value = point.y;
            if (count++ == 0) first = value;
            last = value;
            low = std::min(low, value);
            high = std::max(high, value);
            sum += value;
            squareSum += value * value;
            const auto bin = static_cast<std::size_t>(std::clamp(
                (t - (end - window)) / window * static_cast<double>(width),
                0.0, static_cast<double>(width - 1)));
            bucketSum[bin] += value;
            bucketTimeSum[bin] += t;
            if (bucketCount[bin]++ == 0) bucketMin[bin] = bucketMax[bin] = value;
            else {
                bucketMin[bin] = std::min(bucketMin[bin], value);
                bucketMax[bin] = std::max(bucketMax[bin], value);
            }
        }
        const double mean = sum / count;
        double varianceSum = 0.0, timeSum = 0.0, timeSquareSum = 0.0;
        double timeValueSum = 0.0;
        std::vector<double> intervals, jumps;
        intervals.reserve(count > 0 ? count - 1 : 0);
        jumps.reserve(count > 0 ? count - 1 : 0);
        for (std::size_t i = 0; i < count; ++i) {
            const double x = points[i].t - points.front().t;
            const double dy = points[i].y - mean;
            varianceSum += dy * dy;
            timeSum += x;
            timeSquareSum += x * x;
            timeValueSum += x * dy;
            if (i) {
                const double dt = points[i].t - points[i - 1].t;
                if (dt > 0.0) intervals.push_back(dt);
                jumps.push_back(std::abs(points[i].y - points[i - 1].y));
            }
        }
        const double duration = points.back().t - points.front().t;
        const double stddev = std::sqrt(varianceSum / count);
        const double timeVariance = timeSquareSum - timeSum * timeSum / count;
        const double slope = timeVariance > 0.0 ? timeValueSum / timeVariance : 0.0;
        const double trendR2 = varianceSum > 0.0 && timeVariance > 0.0
            ? std::clamp(slope * slope * timeVariance / varianceSum, 0.0, 1.0) : 0.0;
        const double residualStd = std::sqrt(std::max(
            0.0, varianceSum * (1.0 - trendR2)) / count);
        double earlySum = 0.0, lateSum = 0.0, earlySquares = 0.0, lateSquares = 0.0;
        std::size_t earlyCount = 0, lateCount = 0;
        const double earlyEnd = points.front().t + 0.25 * duration;
        const double lateStart = points.back().t - 0.25 * duration;
        for (const auto& point : points) {
            if (point.t <= earlyEnd) {
                earlySum += point.y;
                earlySquares += point.y * point.y;
                ++earlyCount;
            }
            if (point.t >= lateStart) {
                lateSum += point.y;
                lateSquares += point.y * point.y;
                ++lateCount;
            }
        }
        const double earlyMean = earlySum / earlyCount;
        const double lateMean = lateSum / lateCount;
        const double earlyRms = std::sqrt(earlySquares / earlyCount);
        const double lateRms = std::sqrt(lateSquares / lateCount);
        const double earlyStd = std::sqrt(std::max(0.0,
            earlySquares / earlyCount - earlyMean * earlyMean));
        const double lateStd = std::sqrt(std::max(0.0,
            lateSquares / lateCount - lateMean * lateMean));
        const double medianDt = Median(intervals);
        const double medianJump = Median(jumps);
        const double age = std::max(0.0, end - points.back().t);
        std::size_t gaps = 0, isolatedSpikes = 0;
        double maxGap = 0.0, largestLocalJump = 0.0, stepTime = 0.0;
        for (std::size_t i = 1; i < count; ++i) {
            const double dt = points[i].t - points[i - 1].t;
            maxGap = std::max(maxGap, dt);
            if (medianDt > 0.0 && dt > 3.0 * medianDt) ++gaps;
            if (dt <= 0.0 || (medianDt > 0.0 && dt > 3.0 * medianDt)) continue;
            const double jump = std::abs(points[i].y - points[i - 1].y);
            if (jump > largestLocalJump) {
                largestLocalJump = jump;
                stepTime = 0.5 * (points[i].t + points[i - 1].t);
            }
        }
        const double spikeThreshold = std::max(6.0 * medianJump, 0.2 * (high - low));
        for (std::size_t i = 1; i + 1 < count && spikeThreshold > 0.0; ++i) {
            if (medianDt > 0.0 && (points[i].t - points[i - 1].t > 3.0 * medianDt
                || points[i + 1].t - points[i].t > 3.0 * medianDt)) continue;
            const double left = points[i].y - points[i - 1].y;
            const double right = points[i].y - points[i + 1].y;
            if (left * right > 0.0 && std::abs(left) > spikeThreshold
                && std::abs(right) > spikeThreshold
                && std::abs(points[i - 1].y - points[i + 1].y) < spikeThreshold)
                ++isolatedSpikes;
        }
        std::string spark, spread;
        const double span = high - low;
        double maxSpread = 0.0;
        for (std::size_t i = 0; i < width; ++i)
            if (bucketCount[i]) maxSpread = std::max(maxSpread, bucketMax[i] - bucketMin[i]);
        for (std::size_t i = 0; i < width; ++i) {
            if (!bucketCount[i]) { spark += "·"; spread += " "; continue; }
            const double bucketMean = bucketSum[i] / bucketCount[i];
            const auto level = static_cast<std::size_t>(std::clamp(
                span > 0.0 ? (bucketMean - low) / span * 7.0 : 3.0, 0.0, 7.0));
            spark += levels[level];
            const auto spreadLevel = static_cast<std::size_t>(std::clamp(
                maxSpread > 0.0 ? (bucketMax[i] - bucketMin[i]) / maxSpread * 7.0 : 0.0,
                0.0, 7.0));
            spread += levels[spreadLevel];
        }
        // Autocorrelation of detrended bin means identifies a repeated shape
        // only when at least three cycles fit in the observed window. This is
        // descriptive, not a vibration diagnosis; telemetry can alias faster
        // motor or PWM frequencies.
        std::array<double, width> residual{};
        std::array<bool, width> occupied{};
        std::size_t occupiedBins = 0;
        for (std::size_t i = 0; i < width; ++i) {
            if (!bucketCount[i]) continue;
            const double binTime = bucketTimeSum[i] / bucketCount[i];
            residual[i] = bucketSum[i] / bucketCount[i]
                - (mean + slope * (binTime - points.front().t - timeSum / count));
            occupied[i] = true;
            ++occupiedBins;
        }
        bool passedNegativeLobe = false;
        double oscillationHz = 0.0, bestCorrelation = 0.0;
        std::size_t detectedLag = 0;
        for (std::size_t lag = 2;
             lag <= width / 3 && occupiedBins >= 2 * width / 3
                 && residualStd > 0.05 * stddev
                 && residualStd > 1e-9 * std::max(std::abs(mean), span);
             ++lag) {
            double x = 0.0, y = 0.0, xx = 0.0, yy = 0.0, xy = 0.0;
            std::size_t pairs = 0;
            for (std::size_t i = 0; i + lag < width; ++i) {
                if (!occupied[i] || !occupied[i + lag]) continue;
                x += residual[i];
                y += residual[i + lag];
                xx += residual[i] * residual[i];
                yy += residual[i + lag] * residual[i + lag];
                xy += residual[i] * residual[i + lag];
                ++pairs;
            }
            if (pairs < width / 3) continue;
            const double varianceX = xx - x * x / pairs;
            const double varianceY = yy - y * y / pairs;
            if (varianceX <= 0.0 || varianceY <= 0.0) continue;
            const double correlation = (xy - x * y / pairs)
                / std::sqrt(varianceX * varianceY);
            if (correlation < -0.2) passedNegativeLobe = true;
            if (passedNegativeLobe && oscillationHz == 0.0 && correlation > 0.7
                && medianDt > 0.0
                && (width / (lag * window)) < 0.45 / medianDt) {
                bestCorrelation = correlation;
                oscillationHz = width / (lag * window);
                detectedLag = lag;
            }
        }
        const bool enoughData = count >= 8 && duration > 0.0;
        const bool step = enoughData && span > 0.0
            && largestLocalJump >= 0.6 * span
            && std::abs(lateMean - earlyMean) >= 0.6 * span
            && earlyStd <= 0.15 * span && lateStd <= 0.15 * span;
        std::string pattern = "insufficient_samples";
        if (enoughData) {
            if (oscillationHz > 0.0) pattern = "oscillating";
            else if (step) pattern = lateMean > earlyMean ? "step_up" : "step_down";
            else if (isolatedSpikes) pattern = "isolated_spikes";
            else if (trendR2 >= 0.65 && std::abs(slope * duration) >= 0.35 * span)
                pattern = slope > 0.0 ? "rising" : "falling";
            else if (std::abs(lateMean - earlyMean) <= 0.25 * stddev)
                pattern = "steady_mean";
            else pattern = "variable";
        }
        if (stopped) pattern = reportingState;
        const bool limited = stopped || !enoughData || invalidSamples > 0 || timestampResets > 0
            || duration < 0.5 * window
            || occupiedBins < 2 * width / 3
            || maxGap > 0.1 * window || gaps > count / 10
            || age > std::max(0.1 * window, 3.0 * medianDt);
        const double maxAnalyzedHz = medianDt > 0.0
            ? std::min(width / (2.0 * window), 0.45 / medianDt) : 0.0;
        std::ostringstream interpretation;
        interpretation << pattern << "; slope=" << slope << "/s (R²=" << trendR2
                       << "); early→late mean=" << earlyMean << "→" << lateMean;
        if (oscillationHz > 0.0)
            interpretation << "; repeated pattern ≈" << oscillationHz << " Hz";
        if (step) interpretation << "; step near t=" << stepTime << " s";
        if (isolatedSpikes) interpretation << "; isolated spikes=" << isolatedSpikes;
        if (gaps) interpretation << "; sampling gaps=" << gaps;
        if (invalidSamples) interpretation << "; invalid samples=" << invalidSamples;
        if (timestampResets) interpretation << "; timestamp resets=" << timestampResets;
        if (stopped) interpretation << "; last measured values only";
        if (limited) interpretation << "; limited window coverage or samples";

        chart << name << "  " << spark
              << (stopped ? "  last_measured=" : "  last=") << last
              << "  min=" << low << "  max=" << high << "  rms="
              << std::sqrt(squareSum / count) << "  Δ=" << (last - first) << '\n'
              << "  " << interpretation.str() << '\n';
        if (maxSpread > 0.0) chart << "  intra-bin range " << spread << '\n';
        metrics[name] = {{"samples", count}, {"invalid_samples", invalidSamples},
                         {"timestamp_resets", timestampResets},
                         {"first", first}, {"last", last},
                         {"last_measured", last},
                         {"current_value", stopped ? json(nullptr) : json(last)},
                         {"min", low}, {"max", high}, {"mean", mean},
                         {"rms", std::sqrt(squareSum / count)}, {"delta", last - first},
                         {"stddev", stddev}, {"slope_per_s", slope},
                         {"trend_r2", trendR2}, {"early_mean", earlyMean},
                         {"late_mean", lateMean}, {"mean_shift", lateMean - earlyMean},
                         {"early_rms", earlyRms}, {"late_rms", lateRms},
                         {"early_stddev", earlyStd}, {"late_stddev", lateStd},
                         {"early_samples", earlyCount}, {"late_samples", lateCount},
                         {"rms_shift", lateRms - earlyRms},
                         {"duration_s", duration}, {"age_at_window_end_s", age},
                         {"coverage_fraction", window > 0.0 ? duration / window : 0.0},
                         {"quality", limited ? "limited" : "adequate"},
                         {"reporting_state", reportingState},
                         {"median_sample_interval_s", medianDt},
                         {"sample_rate_hz", medianDt > 0.0 ? 1.0 / medianDt : 0.0},
                         {"sampling_gaps", gaps}, {"max_sample_gap_s", maxGap},
                         {"occupied_bins", occupiedBins},
                         {"isolated_spikes", isolatedSpikes},
                         {"step_time_s", step ? json(stepTime) : json(nullptr)},
                         {"oscillation_hz", oscillationHz > 0.0 ? json(oscillationHz) : json(nullptr)},
                         {"oscillation_lag_bins", detectedLag},
                         {"oscillation_search_band_hz", maxAnalyzedHz > 3.0 / window
                             ? json{{"min", 3.0 / window}, {"max", maxAnalyzedHz}}
                             : json(nullptr)},
                         {"oscillation_correlation", bestCorrelation},
                         {"pattern", pattern}, {"interpretation", interpretation.str()},
                         {"sparkline", spark}, {"range_sparkline", spread}};
    }
    const json report = {{"window_s", window}, {"window_end_s", end},
                         {"metrics", metrics}, {"chart", chart.str()},
                         {"missing", histories.value("missing", json::array())},
                         {"signal_status", signalStatus},
                         {"note", "Stopped signals have null current_value; last_measured and historical metrics are old samples, not a measured zero. Mean, RMS, and regression use received samples without interpolation; sampling gaps can bias them. Pattern labels describe sampled telemetry only; absence of detected oscillation does not rule out faster motor or PWM behavior."}};
    json response = McpText(chart.str());
    response["structuredContent"] = report;
    return response;
}

json CallMcpTool(const std::string& name, const json& arguments,
                 const fs::path& workspace, const fs::path& sessionPath,
                 const std::string& commandSource = "mcp") {
    if (const json invalid = ValidateToolArguments(name, arguments); !invalid.is_null()) return invalid;
    if (name == "rte_project_info") return McpJson(ProjectInfo(workspace));
    if (name == "rte_bridge_ports") {
        const auto ports = RTEAutomation::DiscoverGen7BridgePorts();
        return McpJson({{"bridge", ports.bridge}, {"control", ports.control}});
    }
    if (name == "rte_graph_read") {
        const auto path = WorkspaceFile(workspace, arguments["graph"].get<std::string>());
        if (!path) return McpText("graph file is missing or outside the workspace", true);
        std::ifstream input(*path);
        std::ostringstream contents;
        contents << input.rdbuf();
        return McpText(contents.str());
    }
    if (name == "rte_device_mode")
        return McpJson(ProbeMainMode(arguments.value("serial", ""),
                                     arguments.value("control_port", ""), sessionPath,
                                     arguments.value("probe_bootloader", false),
                                     arguments.value("programmer", "")));
    if (name.rfind("rte_device_", 0) != 0 && name != "rte_build_info"
        && name != "rte_signal_info" && name != "rte_control_status"
        && name != "rte_spike_capture")
        return RunCliTool(name, arguments, workspace, sessionPath);
    std::string method;
    json params = json::object();
    if (name == "rte_device_status") method = "device.status";
    else if (name == "rte_device_telemetry" || name == "rte_device_signal") method = "device.telemetry";
    else if (name == "rte_build_info") method = "device.build_info";
    else if (name == "rte_signal_info") {
        method = "device.catalog";
        params = {{"filter", arguments.value("filter", "")},
                  {"signal", arguments.value("signal", "")}};
    } else if (name == "rte_control_status") method = "device.control_status";
    else if (name == "rte_device_snapshot") {
        method = "device.snapshot";
        params = {{"bundle", arguments.value("bundle", "foc")},
                  {"signals", arguments.value("signals", json::array())}};
    }
    else if (name == "rte_device_histories" || name == "rte_device_trends") {
        method = "device.histories";
        std::vector<std::string> signals;
        if (arguments.contains("signals")) signals = arguments["signals"].get<std::vector<std::string>>();
        if (signals.empty() && name == "rte_device_trends") {
            std::string discoverError;
            const auto session = RTEAutomation::DiscoverSession(sessionPath, &discoverError);
            if (!session) return McpText(discoverError, true);
            const auto telemetry = RTEAutomation::RequestSession(*session, "device.telemetry",
                json::object(), &discoverError);
            if (!telemetry) return McpText(discoverError, true);
            const auto& available = telemetry->at("signals");
            const std::array<std::vector<std::string>, 2> focGroups = {{
                {"cg_id_a", "cg_iq_a", "cg_vd_v", "cg_vq_v", "cg_theta_rad", "cg_vdc_v", "cg_iu_a", "cg_iv_a"},
                {"foc_id", "foc_iq", "foc_vd", "foc_vq", "foc_elec_angle", "foc_vdc", "foc_iu", "foc_iv"}}};
            for (const auto& group : focGroups) {
                for (const auto& key : group)
                    if (available.contains(key)) signals.push_back(key);
                if (!signals.empty()) break;
            }
            if (signals.empty()) {
                for (auto it = available.begin(); it != available.end() && signals.size() < 8; ++it)
                    signals.push_back(it.key());
            }
        }
        if (signals.empty()) return McpText("no numeric telemetry signals are available", true);
        params = {{"signals", signals}, {"window_s", arguments.value("window_s", 5.0)},
                  {"limit", arguments.value("limit", 4000)}};
    }
    else if (name == "rte_device_history") {
        method = "device.history";
        params = {{"signal", arguments["signal"]}, {"limit", arguments.value("limit", 1000)}};
    } else if (name == "rte_device_string_history") {
        method = "device.string_history";
        params = {{"signal", arguments["signal"]}, {"limit", arguments.value("limit", 1000)}};
    }
    else if (name == "rte_device_console") {
        method = "device.console";
        params = {{"since", arguments.value("since", std::uint64_t{0})},
                  {"lines", arguments.value("lines", std::size_t{100})}};
    } else if (name == "rte_device_command" || name == "rte_device_command_response") {
        method = "device.command";
        params = {{"command", arguments["command"]}, {"source", commandSource}};
    } else if (name == "rte_spike_capture") {
        method = "device.command";
        params = {{"command", "spikes"}, {"source", commandSource}};
    } else if (name == "rte_device_commands") {
        method = "device.command";
        params = {{"command", "help"}, {"source", commandSource}};
    } else return McpText("unknown tool: " + name, true);
    std::string error;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &error);
    if (!session) return McpText(error, true);
    const auto result = RTEAutomation::RequestSession(*session, method, params, &error);
    if (!result) return McpText(error, true);
    if (name == "rte_device_trends") return McpTrends(*result);
    if (name == "rte_device_commands") {
        std::uint64_t cursor = result->value("console_since", std::uint64_t{0});
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(arguments.value("timeout_ms", 5000));
        json commands = json::array();
        bool started = false, footerSeen = false;
        std::optional<unsigned> expectedCount;
        auto trim = [](const std::string& value) {
            const auto first = value.find_first_not_of(' ');
            if (first == std::string::npos) return std::string{};
            const auto last = value.find_last_not_of(' ');
            return value.substr(first, last - first + 1);
        };
        while (std::chrono::steady_clock::now() < deadline && !footerSeen) {
            const auto console = RTEAutomation::RequestSession(*session, "device.console",
                {{"since", cursor}, {"lines", 1000}}, &error);
            if (!console) return McpText(error, true);
            for (const auto& line : console->value("lines", json::array())) {
                cursor = std::max(cursor, line.value("seq", std::uint64_t{0}));
                const std::string text = line.value("text", "");
                if (text.rfind("[SHELL] ", 0) != 0) continue;
                const std::string body = text.substr(8);
                if (body.rfind("=== Command Reference", 0) == 0) {
                    started = true;
                    commands = json::array();
                    expectedCount.reset();
                    unsigned count = 0;
                    if (std::sscanf(body.c_str(),
                        "=== Command Reference (%u commands) ===", &count) == 1)
                        expectedCount = count;
                    continue;
                }
                if (!started) continue;
                if (body.rfind("================", 0) == 0) {
                    footerSeen = true;
                    break;
                }
                if (body.rfind("        ", 0) == 0) {
                    if (commands.empty()) continue;
                    const std::string detail = trim(body);
                    const auto colon = detail.find(':');
                    if (colon == std::string::npos) continue;
                    const std::string argName = detail.substr(0, colon);
                    const std::string range = trim(detail.substr(colon + 1));
                    for (auto& arg : commands.back()["arguments"]) {
                        if (arg.value("name", "") != argName) continue;
                        arg["range"] = range;
                        arg["type"] = range == "string" ? "string"
                            : range.find('.') != std::string::npos ? "float" : "int";
                        break;
                    }
                    continue;
                }
                const auto separator = body.find(" - ");
                if (separator == std::string::npos) continue;
                const std::string signature = trim(body.substr(0, separator));
                const auto nameEnd = signature.find(' ');
                const std::string commandName = signature.substr(0, nameEnd);
                const std::string usage = nameEnd == std::string::npos
                    ? "" : trim(signature.substr(nameEnd));
                json args = json::array();
                for (std::size_t pos = 0; pos < usage.size();) {
                    const char open = usage[pos];
                    if (open != '<' && open != '[') { ++pos; continue; }
                    const char close = open == '<' ? '>' : ']';
                    const auto end = usage.find(close, pos + 1);
                    if (end == std::string::npos) break;
                    args.push_back({{"name", usage.substr(pos + 1, end - pos - 1)},
                                    {"required", open == '<'}});
                    pos = end + 1;
                }
                commands.push_back({{"name", commandName}, {"usage", usage},
                    {"description", trim(body.substr(separator + 3))},
                    {"arguments", std::move(args)}});
            }
            if (!footerSeen) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        bool rangesComplete = true;
        for (const auto& command : commands)
            for (const auto& arg : command["arguments"])
                if (!arg.contains("range")) rangesComplete = false;
        const bool countVerified = expectedCount && commands.size() == *expectedCount;
        const bool complete = footerSeen && rangesComplete
            && (!expectedCount || countVerified);
        json report = {{"source", "connected firmware help"}, {"complete", complete},
                       {"motor_control_command", "control start"},
                       {"motor_control_role", "main RTE graph motor control"},
                       {"internal_test_command", "foc start <iq_a> [id_a]"},
                       {"internal_test_role", "native FOC diagnostic; internal testing only"},
                       {"count", commands.size()}, {"expected_count", expectedCount
                            ? json(*expectedCount) : json(nullptr)},
                       {"count_verified", countVerified},
                       {"argument_ranges_complete", rangesComplete},
                       {"commands", commands}};
        if (!complete) {
            json response = McpText(started ? "firmware command list was incomplete"
                : "firmware did not answer help", true);
            response["structuredContent"] = std::move(report);
            return response;
        }
        std::ostringstream summary;
        summary << "Motor control: use 'control start' for the MAIN RTE graph motor control.\n"
                << "Internal testing only: 'foc start <iq_a> [id_a]' runs the native FOC diagnostic.\n\n"
                << commands.size() << " commands from connected firmware:\n";
        for (const auto& command : commands)
            summary << command.value("name", "") << ' '
                    << command.value("usage", "") << " — "
                    << command.value("description", "") << '\n';
        json response = McpText(summary.str());
        response["structuredContent"] = std::move(report);
        return response;
    }
    if (name == "rte_spike_capture") {
        std::uint64_t cursor = result->value("console_since", std::uint64_t{0});
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(arguments.value("timeout_ms", 5000));
        json samples = json::array();
        std::string header;
        bool complete = false, noCapture = false;
        while (std::chrono::steady_clock::now() < deadline && !complete && !noCapture) {
            const auto console = RTEAutomation::RequestSession(*session, "device.console",
                {{"since", cursor}, {"lines", 1000}}, &error);
            if (!console) return McpText(error, true);
            for (const auto& line : console->value("lines", json::array())) {
                cursor = std::max(cursor, line.value("seq", std::uint64_t{0}));
                const std::string text = line.value("text", "");
                if (text.find("[SHELL] spikes: no capture") != std::string::npos) {
                    noCapture = true;
                    header = text;
                } else if (text.find("[SHELL] spikes: capture") != std::string::npos) {
                    header = text;
                } else if (text.find("[SHELL] spk") != std::string::npos) {
                    char marker = ' ';
                    unsigned index = 0, encSin = 0, encCos = 0;
                    unsigned long tick = 0;
                    float iu = 0, iv = 0, angle = 0, deltaAngle = 0;
                    float du = 0, dv = 0, dw = 0;
                    if (std::sscanf(text.c_str(),
                        "[SHELL] spk%c%u t=%lu iu=%f iv=%f ang=%f dang=%f du=%f dv=%f dw=%f sin=%u cos=%u",
                        &marker, &index, &tick, &iu, &iv, &angle, &deltaAngle,
                        &du, &dv, &dw, &encSin, &encCos) == 12) {
                        samples.push_back({{"index", index}, {"trigger", marker == '*'},
                            {"time_s", static_cast<double>(index) / 5000.0},
                            {"tick_ms", tick}, {"iu_a", iu}, {"iv_a", iv},
                            {"angle_deg", angle}, {"delta_angle_deg", deltaAngle},
                            {"duty_u_pct", du}, {"duty_v_pct", dv}, {"duty_w_pct", dw},
                            {"encoder_sin_raw", encSin}, {"encoder_cos_raw", encCos}});
                        if (index == 63) complete = true;
                    }
                }
            }
            if (!complete && !noCapture) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (noCapture) return McpJson({{"captured", false}, {"message", header}});
        json series = json::object();
        for (const char* key : {"iu_a", "iv_a", "angle_deg"}) {
            series[key] = json::array();
            for (const auto& sample : samples)
                if (sample.contains(key)) series[key].push_back({
                    {"time_s", sample["time_s"]}, {"value", sample[key]}});
        }
        const json trend = McpTrends({{"series", series}, {"window_end_s", 63.0 / 5000.0},
                                     {"window_s", 64.0 / 5000.0}, {"missing", json::array()}});
        json report = {{"captured", !samples.empty()}, {"complete", complete},
                       {"sample_rate_hz", 5000}, {"header", header},
                       {"samples", samples}, {"trends", trend["structuredContent"]}};
        json response = McpText(header + "\n" + trend["content"][0]["text"].get<std::string>());
        response["structuredContent"] = std::move(report);
        return response;
    }
    if (name == "rte_device_signal") {
        const std::string signal = arguments["signal"];
        const json statuses = result->value("signal_status", json::object());
        const json status = statuses.value(signal, json::object());
        const json age = status.value("age_s", json(nullptr));
        const bool fresh = status.value("fresh", false);
        const json lastKnown = result->value("last_known_values", json::object());
        if (result->at("signals").contains(signal))
            return McpJson({{"signal", signal}, {"kind", "number"},
                            {"value", result->at("signals").at(signal)},
                            {"last_value", lastKnown.value(signal,
                                result->at("signals").at(signal))},
                            {"age_s", age}, {"fresh", fresh},
                            {"state", status.value("state", "unknown")}});
        if (result->at("strings").contains(signal))
            return McpJson({{"signal", signal}, {"kind", "string"},
                            {"value", result->at("strings").at(signal)},
                            {"last_value", lastKnown.value(signal,
                                result->at("strings").at(signal))},
                            {"age_s", age}, {"fresh", fresh},
                            {"state", status.value("state", "unknown")}});
        const auto catalog = RTEAutomation::RequestSession(*session, "device.catalog",
            {{"signal", signal}}, &error);
        const std::string state = catalog && !catalog->value("signals", json::array()).empty()
            ? catalog->at("signals")[0].value("state", "unknown")
            : catalog ? catalog->value("state", "unknown") : "unknown";
        json response = McpText("telemetry signal unavailable: " + signal + " (" + state + ")", true);
        response["structuredContent"] = {{"code", state == "not_in_build"
            ? "not_in_build" : state == "configured_not_streaming"
            ? "configured_not_streaming" : "unknown_signal"},
            {"signal", signal}, {"state", state}};
        return response;
    }
    if (name != "rte_device_command_response") return McpJson(*result);
    const auto since = result->value("console_since", std::uint64_t{0});
    const int timeout = arguments.value("timeout_ms", 1500);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    json observed = json::array();
    bool responseObserved = false;
    do {
        const auto console = RTEAutomation::RequestSession(*session, "device.console",
            {{"since", since}, {"lines", 1000}}, &error);
        if (!console) return McpText(error, true);
        observed = console->value("lines", json::array());
        responseObserved = false;
        for (const auto& line : observed) {
            const std::string text = line.value("text", "");
            if (text.rfind("[MCP] ", 0) != 0 && text.rfind("[CLI] ", 0) != 0
                && text.rfind("[API] ", 0) != 0
                && text != "(sent)" && text != "> " + arguments["command"].get<std::string>())
                responseObserved = true;
        }
        if (responseObserved || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (true);
    return McpJson({{"command", arguments["command"]}, {"sent", true},
                        {"console_since", since}, {"observed_lines", observed},
                        {"response_observed", responseObserved},
                        {"correlation", "lines received after command; unrelated output may be included"}});
}

int Tool(const std::vector<std::string>& args, Format format) {
    if (args.empty()) {
        std::cerr << "rte tool: MCP tool name is required\n";
        return 2;
    }
    const std::string name = args.front();
    json arguments = json::object();
    fs::path workspace = fs::current_path();
    fs::path sessionPath;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (i + 1 >= args.size()) {
            std::cerr << "rte tool: missing value for " << args[i] << '\n';
            return 2;
        }
        if (args[i] == "--arguments") {
            try { arguments = json::parse(args[++i]); }
            catch (const std::exception& error) {
                std::cerr << "rte tool: invalid JSON arguments: " << error.what() << '\n';
                return 2;
            }
        } else if (args[i] == "--workspace") workspace = args[++i];
        else if (args[i] == "--session") sessionPath = args[++i];
        else {
            std::cerr << "rte tool: unknown option: " << args[i] << '\n';
            return 2;
        }
    }
    std::error_code workspaceError;
    workspace = fs::weakly_canonical(workspace, workspaceError);
    if (workspaceError || !fs::is_directory(workspace)) {
        std::cerr << "rte tool: workspace is not a directory\n";
        return 2;
    }
    json response;
    try { response = CallMcpTool(name, arguments, workspace, sessionPath, "cli"); }
    catch (const std::exception& error) { response = McpText(error.what(), true); }
    const bool failed = response.value("isError", false);
    if (format == Format::Text) {
        for (const auto& block : response.value("content", json::array()))
            if (block.value("type", "") == "text")
                std::cout << block.value("text", "") << '\n';
    } else {
        Emit(format, {{"event", "tool"}, {"name", name}, {"response", response}});
    }
    return failed ? 5 : 0;
}

int Mcp(const std::vector<std::string>& args) {
    fs::path workspace = fs::current_path();
    fs::path sessionPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--workspace" && i + 1 < args.size()) workspace = args[++i];
        else if (args[i] == "--session" && i + 1 < args.size()) sessionPath = args[++i];
        else { std::cerr << "rte mcp: unknown or incomplete option: " << args[i] << '\n'; return 2; }
    }
    std::error_code workspaceError;
    workspace = fs::weakly_canonical(workspace, workspaceError);
    if (workspaceError || !fs::is_directory(workspace)) {
        std::cerr << "rte mcp: workspace is not a directory\n";
        return 2;
    }
    std::string line;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastReadAudit;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json id = nullptr;
        try {
            const json request = json::parse(line);
            if (!request.is_object() || request.value("jsonrpc", "") != "2.0"
                || !request.contains("method") || !request["method"].is_string()) {
                const json invalidId = request.is_object() && request.contains("id")
                    ? request["id"] : json();
                std::cout << json{{"jsonrpc","2.0"},{"id",invalidId},
                    {"error",{{"code",-32600},{"message","invalid JSON-RPC request"}}}}.dump() << '\n';
                std::cout.flush();
                continue;
            }
            if (!request.contains("id")) continue;
            id = request["id"];
            const std::string method = request.value("method", "");
            json result;
            if (method == "initialize") {
                result = {{"protocolVersion", "2025-03-26"},
                          {"capabilities", {{"tools", json::object()},
                                            {"resources", json::object()},
                                            {"prompts", json::object()}}},
                          {"instructions", "RTE Studio owns the live inverter connection. Use rte_device_mode to check Gen7 app or bootloader responsiveness without changing firmware. Use rte_device_telemetry for all latest values and rte_device_console for replies. Simulation is not correctly implemented and is not advised for control or hardware decisions. Flashing controls real hardware."},
                          {"serverInfo", {{"name", "rte"}, {"version", "0.2.0"}}}};
            } else if (method == "tools/list") {
                result = {{"tools", McpTools()}};
            } else if (method == "tools/call") {
                const json params = request.value("params", json::object());
                const std::string name = params.value("name", "");
                const json arguments = params.value("arguments", json::object());
                if (const json invalid = ValidateToolArguments(name, arguments); !invalid.is_null()) {
                    result = invalid;
                } else {
                    const bool frequentRead = IsFrequentMcpRead(name);
                    const auto now = std::chrono::steady_clock::now();
                    bool record = true;
                    if (frequentRead) {
                        const auto previous = lastReadAudit.find(name);
                        record = previous == lastReadAudit.end()
                            || now - previous->second >= std::chrono::seconds(5);
                        if (record) lastReadAudit[name] = now;
                    }
                    if (record) RecordMcpActivity(sessionPath, name, arguments, "started");
                    result = CallMcpTool(name, arguments, workspace, sessionPath);
                    if (!frequentRead || (record && result.value("isError", false))) {
                        RecordMcpActivity(sessionPath, name, json::object(),
                                          result.value("isError", false) ? "failed" : "completed");
                    }
                }
            } else if (method == "resources/list") {
                json resources = json::array();
                if (fs::is_regular_file(workspace / "README.md"))
                    resources.push_back({{"uri", "rte://workspace/README.md"},
                                         {"name", "README.md"}, {"mimeType", "text/markdown"}});
                const json project = ProjectInfo(workspace);
                for (const auto& graph : project["graphs"])
                    resources.push_back({{"uri", "rte://workspace/" + graph.get<std::string>()},
                                         {"name", graph}, {"mimeType", "application/json"}});
                result = {{"resources", std::move(resources)}};
            } else if (method == "resources/read") {
                const json params = request.value("params", json::object());
                const std::string uri = params.value("uri", "");
                const std::string prefix = "rte://workspace/";
                const auto path = uri.rfind(prefix, 0) == 0
                    ? WorkspaceFile(workspace, uri.substr(prefix.size())) : std::nullopt;
                if (!path) {
                    std::cout << json{{"jsonrpc","2.0"},{"id",id},
                        {"error",{{"code",-32602},{"message","resource missing or outside workspace"}}}}.dump() << '\n';
                    std::cout.flush();
                    continue;
                }
                std::ifstream input(*path);
                std::ostringstream contents;
                contents << input.rdbuf();
                RecordMcpActivity(sessionPath, "resources/read", {{"uri", uri}}, "completed");
                result = {{"contents", json::array({{{"uri", uri},
                    {"mimeType", path->extension() == ".json" ? "application/json" : "text/markdown"},
                    {"text", contents.str()}}})}};
            } else if (method == "prompts/list") {
                result = {{"prompts", json::array({{{"name", "rte_inverter_diagnostics"},
                    {"description", "Inspect live inverter status, telemetry, console, and a graph before suggesting a test."}}})}};
            } else if (method == "prompts/get") {
                const json params = request.value("params", json::object());
                if (params.value("name", "") != "rte_inverter_diagnostics") {
                    std::cout << json{{"jsonrpc","2.0"},{"id",id},
                        {"error",{{"code",-32602},{"message","unknown prompt"}}}}.dump() << '\n';
                    std::cout.flush();
                    continue;
                }
                result = {{"description", "Diagnose an RTE inverter"},
                          {"messages", json::array({{{"role", "user"},
                              {"content", {{"type", "text"}, {"text",
                                  "Read RTE device status, all telemetry, and recent console output. Inspect the relevant graph and validate it. Report observations before sending commands or flashing firmware."}}}}})}};
            } else if (method == "ping") {
                result = json::object();
            } else {
                std::cout << json{{"jsonrpc","2.0"},{"id",id},
                    {"error",{{"code",-32601},{"message","method not found"}}}}.dump() << '\n';
                std::cout.flush();
                continue;
            }
            std::cout << json{{"jsonrpc","2.0"},{"id",id},{"result",std::move(result)}}.dump() << '\n';
            std::cout.flush();
        } catch (const json::parse_error& exception) {
            std::cout << json{{"jsonrpc","2.0"},{"id",nullptr},
                {"error",{{"code",-32700},{"message",exception.what()}}}}.dump() << '\n';
            std::cout.flush();
        } catch (const std::exception& exception) {
            std::cout << json{{"jsonrpc","2.0"},{"id",id},
                {"error",{{"code",-32602},{"message",exception.what()}}}}.dump() << '\n';
            std::cout.flush();
        }
    }
    return 0;
}

int Sim(const std::vector<std::string>& args, Format format);

int Dispatch(const Parsed& parsed) {
    if (parsed.command == "version") {
        Emit(parsed.format, {{"event","version"},{"version","0.1.0"}});
        return 0;
    }
    Options options;
    std::string error;
    if (parsed.command == "validate" || parsed.command == "generate"
        || parsed.command == "build" || parsed.command == "flash") {
        if (!ParseOptions(parsed.args, options, error)) {
            Emit(parsed.format, {{"event","error"},{"message",error}});
            return 2;
        }
        if (parsed.command == "validate") return Validate(options, parsed.format);
        if (parsed.command == "generate") return Generate(options, parsed.format);
        if (parsed.command == "build") return Build(options, parsed.format);
        return Flash(options, parsed.format);
    }
    if (parsed.command == "device") return Device(parsed.args, parsed.format);
    if (parsed.command == "tool") return Tool(parsed.args, parsed.format);
    if (parsed.command == "mcp2221") return Mcp2221(parsed.args, parsed.format);
    if (parsed.command == "sim") return Sim(parsed.args, parsed.format);
    if (parsed.command == "trace") return RunTraceCommand(parsed.args);
    if (parsed.command == "mcp") return Mcp(parsed.args);
    Usage();
    return 2;
}

struct SimOptions {
    std::optional<fs::path> graph;
    std::optional<fs::path> scenario;
    std::optional<fs::path> baseSource;
    std::string name;
    bool live = false;
    bool noBuild = false;
    bool help = false;
    double realtime = 0.0;
    bool realtimeSet = false;
};

void SimUsage() {
    std::cerr
        << "usage: rte sim --graph FILE [--scenario FILE] [--base-source DIR]\n"
        << "               [--name NAME] [--live] [--realtime F] [--no-build]\n"
        << "               [--output-format text|json|jsonl]\n"
        << "WARNING: simulation is not correctly implemented; use is not advised.\n";
}

bool ParseSimOptions(const std::vector<std::string>& args, SimOptions& options,
                     Format& format, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto nextPath = [&](const char* name, std::optional<fs::path>& target) {
            if (i + 1 >= args.size()) { error = std::string("missing value for ") + name; return false; }
            target = fs::path(args[++i]);
            return true;
        };
        auto nextString = [&](const char* name, std::string& target) {
            if (i + 1 >= args.size()) { error = std::string("missing value for ") + name; return false; }
            target = args[++i];
            return true;
        };
        if (arg == "--graph") { if (!nextPath("--graph", options.graph)) return false; }
        else if (arg == "--scenario") { if (!nextPath("--scenario", options.scenario)) return false; }
        else if (arg == "--base-source") { if (!nextPath("--base-source", options.baseSource)) return false; }
        else if (arg == "--name") { if (!nextString("--name", options.name)) return false; }
        else if (arg == "--realtime") {
            std::string value;
            if (!nextString("--realtime", value)) return false;
            char* end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end != value.c_str() + value.size() || !std::isfinite(parsed) || parsed < 0.0) {
                error = "invalid --realtime value: " + value;
                return false;
            }
            options.realtime = parsed;
            options.realtimeSet = true;
        }
        else if (arg == "--live") options.live = true;
        else if (arg == "--no-build") options.noBuild = true;
        else if (arg == "--output-format") {
            std::string value;
            if (!nextString("--output-format", value)) return false;
            if (value == "text") format = Format::Text;
            else if (value == "json") format = Format::Json;
            else if (value == "jsonl") format = Format::JsonLines;
            else { error = "invalid --output-format value: " + value; return false; }
            commandFormatOverride = format;
        }
        else if (arg == "--help" || arg == "-h") options.help = true;
        else { error = "unknown option: " + arg; return false; }
    }

    /* Like ParseOptions(): anchor relative paths to the starting cwd so a
     * foreign working directory cannot reinterpret them. */
    auto normalize = [](std::optional<fs::path>& p) {
        if (!p || p->is_absolute()) return;
        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::absolute(*p, ec), ec);
        if (ec || abs.empty()) abs = fs::absolute(*p);
        p = std::move(abs);
    };
    normalize(options.graph);
    normalize(options.scenario);
    normalize(options.baseSource);
    return true;
}

/* Locates the checkout root by walking up from a known repo-local path and
 * looking for the HostSim image. Returns an empty path when rte runs outside
 * a source checkout (e.g. installed). */
fs::path FindRepoRoot(fs::path start) {
    std::error_code ec;
    while (!start.empty() && start != start.parent_path()) {
        if (fs::is_directory(start / "Images" / "HostSim", ec)) return start;
        start = start.parent_path();
    }
    return {};
}

fs::path FindSimEmitter(const fs::path& exeDir, std::string* ignoredEnv) {
    std::error_code ec;
    if (const char* env = std::getenv("RTE_EMITTER"); env && *env) {
        if (fs::is_regular_file(fs::path(env), ec)) return fs::path(env);
        if (ignoredEnv) *ignoredEnv = env;
    }
    const fs::path sibling = exeDir / RTEAutomation::ExecutableName("RTECodeEmitter");
    if (fs::is_regular_file(sibling, ec)) return sibling;
    if (const auto onPath = RTEAutomation::FindExecutableOnPath("RTECodeEmitter")) return *onPath;
    return {};
}

/* The sim name composes build directory names that are wiped with
 * fs::remove_all, so keep it inside a strict charset and refuse any
 * '.'/'..' path segment (separators are already excluded by the charset,
 * which reduces the segment check to rejecting exactly "." and ".."). */
bool IsValidSimName(const std::string& name) {
    if (name.empty()) return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '.'
                     || c == '-';
        if (!ok) return false;
    }
    return name != "." && name != "..";
}

/* Plain-text scans mirroring HostSim's lenient scenario parser: first
 * "key": match, then the value after the colon. */
std::string ScanJsonString(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t pos = blob.find(needle);
    if (pos == std::string::npos) return {};
    const std::size_t colon = blob.find(':', pos);
    const std::size_t q1 = blob.find('"', colon);
    const std::size_t q2 = q1 == std::string::npos ? q1 : blob.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) return {};
    return blob.substr(q1 + 1, q2 - q1 - 1);
}

std::optional<int> ScanJsonInt(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t pos = blob.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    const std::size_t colon = blob.find(':', pos);
    if (colon == std::string::npos) return std::nullopt;
    const char* start = blob.c_str() + colon + 1;
    char* end = nullptr;
    const long value = std::strtol(start, &end, 10);
    if (end == start) return std::nullopt;
    return static_cast<int>(value);
}

/* Runs a child process, streaming merged stdout/stderr. Text mode relays
 * lines to stdout; structured formats keep stdout clean for the result.
 * On POSIX the child is terminated if rte itself receives SIGINT/SIGTERM/
 * SIGHUP, so an interrupted run never orphans host_sim/emitter/cmake (no-op
 * on Windows — a CREATE_NO_WINDOW child cannot receive Ctrl+C, and Ctrl+C
 * semantics there are left to the console/kill tools). */
bool RunSimStep(const RTEAutomation::ProcessSpec& spec, Format format,
                const std::function<void(const std::string&)>& onLine,
                std::string& error) {
    RTEAutomation::ProcessSpec guarded = spec;
    guarded.terminateWithParent = true;
    const auto result = RTEAutomation::RunProcess(guarded, [&](const std::string& line) {
        if (onLine) onLine(line);
        if (format == Format::Text) std::cout << line << '\n';
        else std::cerr << line << '\n';
    });
    if (!result.started) {
        error = result.error.empty()
            ? "failed to start " + spec.executable.filename().string() : result.error;
        return false;
    }
    if (result.exitCode != 0) {
        error = spec.executable.filename().string() + " " + result.error;
        return false;
    }
    return true;
}

int Sim(const std::vector<std::string>& args, Format format) {
    SimOptions options;
    std::string error;
    if (!ParseSimOptions(args, options, format, error)) {
        Emit(format, {{"event","error"},{"message",error}});
        SimUsage();
        return 2;
    }
    if (options.help) { SimUsage(); return 0; }
    if (!options.graph) {
        Emit(format, {{"event","error"},{"message","--graph is required"}});
        SimUsage();
        return 2;
    }
    std::error_code ec;
    if (!fs::is_regular_file(*options.graph, ec)) {
        Emit(format, {{"event","error"},{"message","graph not found: " + options.graph->string()}});
        return 3;
    }
    Emit(format, {{"event", "warning"},
                  {"message", "Simulation is not correctly implemented and is not advised for control or hardware decisions."}});

    const fs::path exeDir = RTEAutomation::ExecutablePath().parent_path();
    const fs::path repoRoot = FindRepoRoot(exeDir);
    if (!options.baseSource) {
        if (repoRoot.empty()) {
            Emit(format, {{"event","error"},
                          {"message","could not locate the repository root; pass --base-source"}});
            return 2;
        }
        options.baseSource = repoRoot / "Images" / "HostSim";
    }
    if (!fs::is_directory(*options.baseSource, ec)) {
        Emit(format, {{"event","error"},
                      {"message","base source not found: " + options.baseSource->string()}});
        return 3;
    }

    /* rte lives in build/bin in a source checkout; the emitted simulator and
     * its build tree sit next to it under the same build root. With no
     * checkout in sight (installed binary), use the user cache instead of the
     * install prefix's parent, which may be a system directory. */
    fs::path buildRoot;
    if (!repoRoot.empty())
        buildRoot = exeDir.filename() == "bin" ? exeDir.parent_path() : repoRoot / "build";
    else
        buildRoot = RTEAutomation::DefaultCacheRoot() / "sim";

    const std::string name = options.name.empty()
        ? options.graph->stem().string() : options.name;
    if (!IsValidSimName(name)) {
        Emit(format, {{"event","error"},
                      {"message","invalid sim name \"" + name
                         + "\" (allowed: A-Z a-z 0-9 _ . - ; not \".\" or \"..\")"
                         + (options.name.empty() ? "; pass --name" : "")}});
        return 2;
    }
    const fs::path emittedDir = buildRoot / ("hostsim_" + name + "_emitted");
    const fs::path simBuildDir(fs::path(emittedDir).string() + "_build");
    const fs::path runDir = simBuildDir / "run";
    const fs::path simExe = simBuildDir / RTEAutomation::ExecutableName("host_sim");

    fs::path scenario;
    if (options.scenario) {
        scenario = *options.scenario;
    } else {
        /* Same rule as Images/HostSim/scripts/run_spwm_live.sh: scenario
         * named after the graph (minus any trailing _graph), else the
         * generic motor scenario. */
        std::string base = name;
        constexpr std::string_view suffix = "_graph";
        if (base.size() > suffix.size()
            && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
            base.resize(base.size() - suffix.size());
        }
        const fs::path candidate = *options.baseSource / "scenarios" / (base + ".json");
        scenario = fs::is_regular_file(candidate, ec)
            ? candidate : *options.baseSource / "scenarios" / "default_motor.json";
    }
    if (!fs::is_regular_file(scenario, ec)) {
        Emit(format, {{"event","error"},{"message","scenario not found: " + scenario.string()}});
        return 3;
    }

    if (!options.noBuild) {
        std::string ignoredEnv;
        const fs::path emitter = FindSimEmitter(exeDir, &ignoredEnv);
        if (emitter.empty()) {
            std::string message = "RTECodeEmitter not found; build it or set RTE_EMITTER";
            if (!ignoredEnv.empty())
                message += " (RTE_EMITTER points to \"" + ignoredEnv
                         + "\", not an existing file)";
            Emit(format, {{"event","error"},{"message",message}});
            return 3;
        }
        Emit(format, {{"event","progress"},{"phase","sim-emit"},{"percent",10},
                      {"message","Emitting simulation sources"},
                      {"output",emittedDir.string()}});
        fs::remove_all(emittedDir, ec);
        if (ec) {
            Emit(format, {{"event","error"},
                          {"message","could not clear previous emit directory "
                             + emittedDir.string() + ": " + ec.message()}});
            return 4;
        }
        RTEAutomation::ProcessSpec emit;
        emit.executable = emitter;
        emit.arguments = {"--base-src", options.baseSource->string(),
                          "--graph", options.graph->string(),
                          "--output", emittedDir.string(),
                          "--verbosity", format == Format::Text ? "info" : "warning"};
        if (!RunSimStep(emit, format, {}, error)) {
            Emit(format, {{"event","error"},{"message","simulation source generation failed: " + error}});
            return 4;
        }

        Emit(format, {{"event","progress"},{"phase","sim-build"},{"percent",40},
                      {"message","Building host_sim"},
                      {"build",simBuildDir.string()}});
        const unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
        RTEAutomation::ProcessSpec configure;
        configure.executable = "cmake";
        configure.arguments = {"-S", emittedDir.string(), "-B", simBuildDir.string()};
        if (!RunSimStep(configure, format, {}, error)) {
            Emit(format, {{"event","error"},{"message","simulation configure failed: " + error}});
            return 4;
        }
        RTEAutomation::ProcessSpec build;
        build.executable = "cmake";
        build.arguments = {"--build", simBuildDir.string(), "-j", std::to_string(jobs)};
        if (!RunSimStep(build, format, {}, error)) {
            Emit(format, {{"event","error"},{"message","simulation build failed: " + error}});
            return 4;
        }
    }
    if (!fs::is_regular_file(simExe, ec)) {
        Emit(format, {{"event","error"},
                      {"message","host_sim not found at " + simExe.string()
                         + (options.noBuild ? " (--no-build, nothing built)" : "")}});
        return 4;
    }
    Emit(format, {{"event","artifact"},{"kind","sim-binary"},{"path",simExe.string()}});

    /* Batch defaults to as-fast execution; live defaults to wall-clock so the
     * TCP telemetry stream behaves like hardware (same as run_spwm_live.sh). */
    const double realtime = options.realtimeSet ? options.realtime
                                                : (options.live ? 1.0 : 0.0);
    std::ostringstream realtimeText;
    realtimeText << realtime;

    /* The endpoint host_sim binds: its built-in default, overridden by the
     * effective scenario's simulation.listen_host / listen_port. rte passes
     * it to host_sim as --listen because with bare --live host_sim would
     * otherwise pin its CLI defaults over the scenario values. */
    std::string liveHost = "127.0.0.1";
    int livePort = 14608;
    std::ifstream scenarioFile(scenario);
    std::ostringstream scenarioText;
    scenarioText << scenarioFile.rdbuf();
    const std::string scenarioBlob = scenarioText.str();
    if (const std::string host = ScanJsonString(scenarioBlob, "listen_host"); !host.empty())
        liveHost = host;
    if (const auto port = ScanJsonInt(scenarioBlob, "listen_port");
        port && *port > 0 && *port <= 65535)
        livePort = *port;
    const std::string liveEndpoint = liveHost + ":" + std::to_string(livePort);

    if (options.live) {
        Emit(format, {{"event","progress"},{"phase","sim-run"},{"percent",90},
                      {"message","host_sim running in the foreground; press Ctrl+C to stop; "
                                 "live telemetry on " + liveEndpoint + " (IVP)"},
                      {"live",true},{"endpoint",liveEndpoint},{"protocol","ivp"}});
    } else {
        Emit(format, {{"event","progress"},{"phase","sim-run"},{"percent",90},
                      {"message","Running scenario"},{"live",false}});
    }

    fs::create_directories(runDir, ec);
    if (ec) {
        Emit(format, {{"event","error"},
                      {"message","could not create run directory " + runDir.string()
                         + ": " + ec.message()}});
        return 4;
    }

    std::string traceName;
    RTEAutomation::ProcessSpec run;
    run.executable = simExe;
    run.arguments = {scenario.string(), "--realtime", realtimeText.str()};
    if (options.live) run.arguments.insert(run.arguments.end(), {"--live", "--listen", liveEndpoint});
    run.workingDirectory = runDir;
    run.terminateWithParent = true;
    const auto result = RTEAutomation::RunProcess(run, [&](const std::string& line) {
        constexpr std::string_view wrote = "HostSim: wrote ";
        if (line.compare(0, wrote.size(), wrote) == 0) traceName = line.substr(wrote.size());
        if (format == Format::Text) std::cout << line << '\n';
        else std::cerr << line << '\n';
    });
    if (!result.started) {
        Emit(format, {{"event","error"},
                      {"message",result.error.empty() ? "failed to start host_sim"
                                                      : result.error}});
        return 4;
    }
    if (result.exitCode != 0) {
        Emit(format, {{"event","error"},{"message","host_sim " + result.error}});
        return 4;
    }

    json trace;
    if (!traceName.empty()) {
        fs::path tracePath(traceName);
        if (tracePath.is_relative()) tracePath = fs::absolute(runDir / tracePath, ec);
        if (!fs::is_regular_file(tracePath, ec)) {
            Emit(format, {{"event","error"},
                          {"message","host_sim did not produce trace " + tracePath.string()}});
            return 4;
        }
        Emit(format, {{"event","artifact"},{"kind","sim-trace"},
                      {"path",tracePath.string()}});
        trace = tracePath.string();
    }

    Emit(format, {{"event","complete"},{"success",true},
                  {"graph",options.graph->string()},
                  {"scenario",scenario.string()},
                  {"sim_binary",simExe.string()},
                  {"live",options.live},
                  {"realtime",realtime},
                  {"trace",trace}});
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto parsed = ParseTopLevel(argc, argv);
    if (!parsed) { Usage(); return 2; }
    const int exitCode = Dispatch(*parsed);
    if (commandFormatOverride.value_or(parsed->format) == Format::Json) {
        std::cout << json{{"success", exitCode == 0},
                          {"exit_code", exitCode},
                          {"events", jsonEvents}}.dump(2) << '\n';
    }
    return exitCode;
}
