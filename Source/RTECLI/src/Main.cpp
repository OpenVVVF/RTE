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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
        << "  device status|telemetry|console|command|mode [--session FILE]\n"
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
    fs::path sessionPath;
    std::uint64_t since = 0;
    std::size_t lines = 100;
    std::string command;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--session" && i + 1 < args.size()) sessionPath = args[++i];
        else if (args[i] == "--since" && i + 1 < args.size()) {
            try { since = std::stoull(args[++i]); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --since value"}}); return 2; }
        } else if (args[i] == "--lines" && i + 1 < args.size()) {
            try { lines = static_cast<std::size_t>(std::stoull(args[++i])); }
            catch (...) { Emit(format, {{"event","error"},{"message","invalid --lines value"}}); return 2; }
        } else if (args[i] == "--command" && i + 1 < args.size()) command = args[++i];
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
    else if (operation == "console") {
        method = "device.console";
        params = {{"since", since}, {"lines", lines}};
    } else if (operation == "command") {
        if (command.empty()) {
            Emit(format, {{"event","error"},{"message","device command text is required"}});
            return 2;
        }
        method = "device.command";
        params = {{"command", command}};
    } else {
        Emit(format, {{"event","error"},{"message","unknown device subcommand: " + operation}});
        return 2;
    }
    const auto result = RTEAutomation::RequestSession(*session, method, params, &error);
    if (!result) {
        Emit(format, {{"event","error"},{"message",error}});
        return 5;
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
        || name == "rte_device_console";
    const bool destructive = name == "rte_flash" || name == "rte_device_command"
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
        ToolDefinition("rte_flash", "Flash main MCU over the Gen7 UART bridge or coprocessor over USB DFU.",
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
        ToolDefinition("rte_device_telemetry", "Read all latest numeric and string telemetry plus receive statistics.", {}),
        ToolDefinition("rte_device_signal", "Read the latest value of one numeric or string telemetry signal.",
            {{"signal", {{"type", "string"}}}}, {"signal"}),
        ToolDefinition("rte_device_history", "Read recent time and value samples for one numeric telemetry signal.",
            {{"signal", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 12000}}}}, {"signal"}),
        ToolDefinition("rte_device_string_history", "Read recent string telemetry events for one signal.",
            {{"signal", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}, {"signal"}),
        ToolDefinition("rte_device_console", "Read device console lines from RTE Studio.",
            {{"since", {{"type", "integer"}, {"minimum", 0}}},
             {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}),
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
    std::string output;
    const auto result = RTEAutomation::RunProcess(process, [&](const std::string& line) {
        output += line;
        output += '\n';
    });
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
                || (type == "integer" && !it.value().is_number_integer()))
                return McpText("invalid type for argument: " + it.key(), true);
            if (type == "integer") {
                const auto number = it.value().get<std::int64_t>();
                if ((property.contains("minimum") && number < property["minimum"].get<std::int64_t>())
                    || (property.contains("maximum") && number > property["maximum"].get<std::int64_t>()))
                    return McpText("argument out of range: " + it.key(), true);
            }
            if (property.contains("enum") && std::find(property["enum"].begin(), property["enum"].end(), it.value()) == property["enum"].end())
                return McpText("invalid value for argument: " + it.key(), true);
        }
        return json();
    }
    return McpText("unknown tool: " + name, true);
}

json CallMcpTool(const std::string& name, const json& arguments,
                 const fs::path& workspace, const fs::path& sessionPath) {
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
    if (name.rfind("rte_device_", 0) != 0)
        return RunCliTool(name, arguments, workspace, sessionPath);
    std::string method;
    json params = json::object();
    if (name == "rte_device_status") method = "device.status";
    else if (name == "rte_device_telemetry" || name == "rte_device_signal") method = "device.telemetry";
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
        params = {{"command", arguments["command"]}};
    } else return McpText("unknown tool: " + name, true);
    std::string error;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &error);
    if (!session) return McpText(error, true);
    const auto result = RTEAutomation::RequestSession(*session, method, params, &error);
    if (!result) return McpText(error, true);
    if (name == "rte_device_signal") {
        const std::string signal = arguments["signal"];
        if (result->at("signals").contains(signal))
            return McpJson({{"signal", signal}, {"kind", "number"},
                            {"value", result->at("signals").at(signal)}});
        if (result->at("strings").contains(signal))
            return McpJson({{"signal", signal}, {"kind", "string"},
                            {"value", result->at("strings").at(signal)}});
        return McpText("unknown telemetry signal: " + signal, true);
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
            if (text != "(sent)" && text != "> " + arguments["command"].get<std::string>())
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
                result = CallMcpTool(params.value("name", ""),
                                     params.value("arguments", json::object()),
                                     workspace, sessionPath);
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
