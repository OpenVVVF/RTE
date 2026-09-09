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
#include <nlohmann/json.hpp>

#include <cmath>
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
        << "  flash --firmware FILE [--serial PORT | --session FILE] [--manual-boot]\n"
        << "  mcp2221 enter|exit|release\n"
        << "  device status|telemetry|console|command [--session FILE]\n"
        << "  sim --graph FILE [--scenario FILE] [--base-source DIR] [--name NAME]\n"
        << "      [--live] [--realtime F] [--no-build] [--output-format text|json]\n"
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
    std::string buildType = "Release";
    std::string toolchain = "auto";
    std::string generator = "Ninja";
    bool clean = false;
    bool autoGpio = true;
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
        else if (arg == "--build-type") { if (!nextString("--build-type", options.buildType)) return false; }
        else if (arg == "--toolchain") { if (!nextString("--toolchain", options.toolchain)) return false; }
        else if (arg == "--generator") { if (!nextString("--generator", options.generator)) return false; }
        else if (arg == "--clean") options.clean = true;
        else if (arg == "--manual-boot") options.autoGpio = false;
        else if (arg == "--auto-gpio") options.autoGpio = true;
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
    std::string serial = options.serial;
    std::optional<RTEAutomation::SessionDescriptor> session;
    bool studioLease = false;
    std::string sessionError;
    if (serial.empty()) {
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
    if (options.programmer) flash.programmer = *options.programmer;
    flash.autoGpio = options.autoGpio;
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

int Device(const std::vector<std::string>& args, Format format) {
    if (args.empty()) {
        Emit(format, {{"event", "error"}, {"message", "device subcommand is required"}});
        return 2;
    }
    const std::string operation = args.front();
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

json ToolDefinition(const std::string& name, const std::string& description,
                    json properties, std::vector<std::string> required = {}) {
    json schema = {{"type", "object"}, {"properties", std::move(properties)},
                   {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = std::move(required);
    return {{"name", name}, {"description", description}, {"inputSchema", std::move(schema)}};
}

json McpTools() {
    const json path = {{"type", "string"}};
    return json::array({
        ToolDefinition("rte_validate", "Validate an RTE graph without changing files.",
            {{"graph", path}, {"templates", path}}, {"graph"}),
        ToolDefinition("rte_generate", "Generate firmware sources from an RTE graph.",
            {{"graph", path}, {"base_source", path}, {"output", path}, {"templates", path}},
            {"graph", "base_source", "output"}),
        ToolDefinition("rte_build", "Generate and build firmware in the RTE user cache.",
            {{"graph", path}, {"base_source", path}, {"templates", path},
             {"build_type", {{"type", "string"}}}}, {"graph", "base_source"}),
        ToolDefinition("rte_flash", "Flash a firmware binary using the portable RTE worker.",
            {{"firmware", path}, {"serial", {{"type", "string"}}},
             {"manual_boot", {{"type", "boolean"}}}}, {"firmware"}),
        ToolDefinition("rte_device_status", "Read the active RTE Studio device status.", {}),
        ToolDefinition("rte_device_telemetry", "Read latest telemetry from RTE Studio.", {}),
        ToolDefinition("rte_device_console", "Read device console lines from RTE Studio.",
            {{"since", {{"type", "integer"}, {"minimum", 0}}},
             {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}}),
        ToolDefinition("rte_device_command",
            "Send a device command. Requires the disabled-by-default write preference in RTE Studio.",
            {{"command", {{"type", "string"}}}}, {"command"})
    });
}

json RunCliTool(const std::string& tool, const json& arguments,
                const fs::path& workspace) {
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
        if (arguments.value("manual_boot", false)) process.arguments.emplace_back("--manual-boot");
    } else return McpText("unknown tool: " + tool, true);
    std::string output;
    const auto result = RTEAutomation::RunProcess(process, [&](const std::string& line) {
        output += line;
        output += '\n';
    });
    if (!result.started) return McpText(result.error, true);
    return McpText(output.empty() ? (result.exitCode == 0 ? "complete" : "operation failed") : output,
                   result.exitCode != 0);
}

json CallMcpTool(const std::string& name, const json& arguments,
                 const fs::path& workspace, const fs::path& sessionPath) {
    if (name.rfind("rte_device_", 0) != 0) return RunCliTool(name, arguments, workspace);
    std::string method;
    json params = json::object();
    if (name == "rte_device_status") method = "device.status";
    else if (name == "rte_device_telemetry") method = "device.telemetry";
    else if (name == "rte_device_console") {
        method = "device.console";
        params = {{"since", arguments.value("since", std::uint64_t{0})},
                  {"lines", arguments.value("lines", std::size_t{100})}};
    } else if (name == "rte_device_command") {
        method = "device.command";
        params = {{"command", arguments.value("command", "")}};
    } else return McpText("unknown tool: " + name, true);
    std::string error;
    const auto session = RTEAutomation::DiscoverSession(sessionPath, &error);
    if (!session) return McpText(error, true);
    const auto result = RTEAutomation::RequestSession(*session, method, params, &error);
    return result ? McpText(result->dump(2)) : McpText(error, true);
}

int Mcp(const std::vector<std::string>& args) {
    fs::path workspace = fs::current_path();
    fs::path sessionPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--workspace" && i + 1 < args.size()) workspace = args[++i];
        else if (args[i] == "--session" && i + 1 < args.size()) sessionPath = args[++i];
        else { std::cerr << "rte mcp: unknown or incomplete option: " << args[i] << '\n'; return 2; }
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            const json request = json::parse(line);
            if (!request.contains("id")) continue;
            const json id = request["id"];
            const std::string method = request.value("method", "");
            json result;
            if (method == "initialize") {
                result = {{"protocolVersion", "2025-03-26"},
                          {"capabilities", {{"tools", json::object()}}},
                          {"serverInfo", {{"name", "rte"}, {"version", "0.1.0"}}}};
            } else if (method == "tools/list") {
                result = {{"tools", McpTools()}};
            } else if (method == "tools/call") {
                const json params = request.value("params", json::object());
                result = CallMcpTool(params.value("name", ""),
                                     params.value("arguments", json::object()),
                                     workspace, sessionPath);
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
        } catch (const std::exception& exception) {
            std::cout << json{{"jsonrpc","2.0"},{"id",nullptr},
                {"error",{{"code",-32700},{"message",exception.what()}}}}.dump() << '\n';
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
        << "               [--output-format text|json]\n";
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

fs::path FindSimEmitter(const fs::path& exeDir) {
    if (const char* env = std::getenv("RTE_EMITTER"); env && *env) return fs::path(env);
    std::error_code ec;
    const fs::path sibling = exeDir / RTEAutomation::ExecutableName("RTECodeEmitter");
    if (fs::is_regular_file(sibling, ec)) return sibling;
    if (const auto onPath = RTEAutomation::FindExecutableOnPath("RTECodeEmitter")) return *onPath;
    return {};
}

/* Runs a child process, streaming merged stdout/stderr. Text mode relays
 * lines to stdout; structured formats keep stdout clean for the result. */
bool RunSimStep(const RTEAutomation::ProcessSpec& spec, Format format,
                const std::function<void(const std::string&)>& onLine,
                std::string& error) {
    const auto result = RTEAutomation::RunProcess(spec, [&](const std::string& line) {
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
    if (options.help) { SimUsage(); return 2; }
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
     * its build tree sit next to it under the same build root. */
    fs::path buildRoot;
    if (exeDir.filename() == "bin") buildRoot = exeDir.parent_path();
    else if (!repoRoot.empty()) buildRoot = repoRoot / "build";
    else buildRoot = exeDir;

    const std::string name = options.name.empty()
        ? options.graph->stem().string() : options.name;
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
        const fs::path emitter = FindSimEmitter(exeDir);
        if (emitter.empty() || !fs::is_regular_file(emitter, ec)) {
            Emit(format, {{"event","error"},
                          {"message","RTECodeEmitter not found; build it or set RTE_EMITTER"}});
            return 3;
        }
        Emit(format, {{"event","progress"},{"phase","sim-emit"},{"percent",10},
                      {"message","Emitting simulation sources"},
                      {"output",emittedDir.string()}});
        fs::remove_all(emittedDir, ec);
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

    if (options.live) {
        Emit(format, {{"event","progress"},{"phase","sim-run"},{"percent",90},
                      {"message","host_sim running in the foreground; press Ctrl+C to stop"},
                      {"live",true},{"endpoint","127.0.0.1:14608"},{"protocol","ivp"}});
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
    if (options.live) run.arguments.emplace_back("--live");
    run.workingDirectory = runDir;
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
