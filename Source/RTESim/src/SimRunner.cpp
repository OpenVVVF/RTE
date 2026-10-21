#include "RTESim/SimRunner.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "Emitter.h"                       // via RTEAutomation (RTECodeEmitter)
#include <RTEAutomation/ProcessRunner.h>
#include <NodeAPI/NodeTemplates.h>
#include <NodeAPI/Serialization.h>

#ifdef RTESIM_HAVE_PLANTCODEGEN
#include <PlantCodegen/PlantGenerator.h>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace RTESim {

namespace {

[[maybe_unused]]
bool ReadTextFile(const fs::path& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool CopyFile(const fs::path& src, const fs::path& dst, std::string& error) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "could not copy " + src.string() + " -> " + dst.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Walk up from `start` looking for a directory that holds
// Assets/PlantTemplates; used so `rte sim` works from any cwd without a flag.
fs::path FindUpwards(fs::path start, const fs::path& needle) {
    std::error_code ec;
    start = fs::weakly_canonical(fs::absolute(start, ec), ec);
    for (int depth = 0; depth < 8 && !start.empty(); ++depth) {
        if (fs::is_directory(start / needle, ec)) return start / needle;
        const fs::path parent = start.parent_path();
        if (parent == start) break;
        start = parent;
    }
    return {};
}

fs::path ExecutableDir() {
#if defined(__linux__)
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.parent_path();
#endif
    return {};
}

} // namespace

SimLayout LayoutForWorkDir(const fs::path& workDir) {
    SimLayout layout;
    layout.sw = workDir / "sw";
    layout.plant = workDir / "plant";
    layout.host = workDir / "host";
    layout.binary = layout.host / "sim_host";
    return layout;
}

SimRunner::SimRunner(SimOptions options, Logger logger)
    : options_(std::move(options)), logger_(std::move(logger)) {}

bool SimRunner::Run(std::string& error) {
    // ---- validate + resolve defaults ----
    if (options_.swGraphPath.empty() || !fs::exists(options_.swGraphPath)) {
        error = "sw graph not found: " + options_.swGraphPath.string();
        return false;
    }
    if (options_.plantGraphPath.empty() || !fs::exists(options_.plantGraphPath)) {
        error = "plant graph not found: " + options_.plantGraphPath.string();
        return false;
    }
    if (options_.baseSourceDir.empty() || !fs::is_directory(options_.baseSourceDir)) {
        error = "base source dir not found: " + options_.baseSourceDir.string();
        return false;
    }
    if (options_.outPath.empty()) {
        error = "output path (outPath) is required";
        return false;
    }
    if (options_.plantTemplatesDir.empty()) {
        if (const char* env = std::getenv("RTESIM_PLANT_TEMPLATES")) {
            options_.plantTemplatesDir = env;
        } else {
            fs::path found = FindUpwards(fs::current_path(), "Assets/PlantTemplates");
            if (found.empty()) {
                const fs::path exeDir = ExecutableDir();
                if (!exeDir.empty()) found = FindUpwards(exeDir, "Assets/PlantTemplates");
            }
            if (!found.empty()) options_.plantTemplatesDir = found;
        }
    }
#ifndef RTESIM_HAVE_PLANTCODEGEN
    (void)options_;   // plant graph is honored via the fallback below
#else
    if (options_.plantTemplatesDir.empty() || !fs::is_directory(options_.plantTemplatesDir)) {
        error = "plant templates dir not found (set --plant-templates-dir or "
                "RTESIM_PLANT_TEMPLATES): " + options_.plantTemplatesDir.string();
        return false;
    }
#endif

    if (options_.workDir.empty()) {
        options_.workDir = options_.outPath;
        options_.workDir += ".work";
    }
    layout_ = LayoutForWorkDir(options_.workDir);

    if (logger_) logger_("workdir: " + options_.workDir.string());

    // F28: --dyno-rpm and --dyno-series name the same knob; both is a usage
    // error (SimCli also gates this; defense for direct API users).
    if (options_.dynoRpmSet && !options_.dynoSeries.empty()) {
        error = "dyno-rpm and dyno-series are mutually exclusive";
        return false;
    }

    if (!EmitSw(error)) return false;
    if (!GeneratePlant(error)) return false;
    if (!StageHostSources(error)) return false;
    if (!CompileHost(error)) return false;
    if (!RunHost(error)) return false;
    return true;
}

/* --------------------------------------------------------------------------
 * 1. SW emit: EXACTLY the rte generate pipeline (RTECodeEmitter::Emitter)
 * -------------------------------------------------------------------------- */

bool SimRunner::EmitSw(std::string& error) {
    if (logger_) logger_("emitting controller firmware: " + options_.swGraphPath.string() +
                        " -> " + layout_.sw.string());
    RTECodeEmitter::Logger logger(RTECodeEmitter::LogLevel::Warning);
    RTECodeEmitter::EmitterOptions emitter;
    emitter.graphPath = options_.swGraphPath;
    emitter.baseSrc = options_.baseSourceDir;
    emitter.outputDir = layout_.sw;
    if (!options_.templatesDir.empty()) emitter.templatesDir = options_.templatesDir;
    if (!RTECodeEmitter::Emitter(logger).Run(emitter)) {
        error = "controller firmware emission failed";
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 2. Plant generation: PlantCodegen when part of the build, else fallback
 * -------------------------------------------------------------------------- */

bool SimRunner::GeneratePlant(std::string& error) {
#ifdef RTESIM_HAVE_PLANTCODEGEN
    if (logger_) logger_("generating plant from graph: " + options_.plantGraphPath.string());
    NodeAPI::Graph graph;
    auto loaded = NodeAPI::LoadNodeTypesFromDirectory(graph, options_.plantTemplatesDir);
    if (!loaded.ok) {
        std::ostringstream text;
        text << "plant template loading failed";
        for (const auto& item : loaded.errors) text << "; " << item;
        error = text.str();
        return false;
    }
    std::string graphText;
    if (!ReadTextFile(options_.plantGraphPath, graphText)) {
        error = "could not open plant graph: " + options_.plantGraphPath.string();
        return false;
    }
    try {
        // The example plant graphs embed no "nodeTypes" array (they reference
        // the shared Assets/PlantTemplates library loaded above); the
        // sanctioned loader requires the key. Mirrors
        // Lib/PlantCodegen/tests/plant_test_util.h LoadPlantGraph.
        nlohmann::json parsed = nlohmann::json::parse(graphText);
        if (!parsed.contains("nodeTypes")) parsed["nodeTypes"] = nlohmann::json::array();
        NodeAPI::LoadIntoGraph(graph, parsed.dump());
    } catch (const std::exception& e) {
        error = std::string("could not parse plant graph: ") + e.what();
        return false;
    }
    std::error_code ec;
    fs::create_directories(layout_.plant, ec);
    std::string genError;
    std::vector<std::string> genWarnings;
    PlantCodegen::PlantGenerator generator(graph);
    if (!generator.Generate(layout_.plant.string(), genError, &genWarnings)) {
        error = "plant code generation failed: " + genError;
        return false;
    }
    if (logger_) {
        for (const auto& w : genWarnings) logger_("plant-codegen warning: " + w);
    }
    return true;
#else
    // F15: the fallback plant is a fixed Gen7-bench physics substitute for
    // whatever graph the user passed — running it must be an explicit opt-in,
    // never the silent default.
    if (!options_.allowFallbackPlant) {
        error = "this rte build has no PlantCodegenLib, so it cannot generate "
                "the plant from " + options_.plantGraphPath.string() +
                "; re-run with --allow-fallback-plant to opt into the built-in "
                "Gen7-bench fallback plant";
        return false;
    }
    if (logger_) {
        logger_("PlantCodegenLib not in this build; staging built-in fallback plant "
               "(Gen7 Size2 bench) for " + options_.plantGraphPath.string());
    }
    const fs::path srcDir = fs::path(RTESIM_RESOURCE_DIR) / "fallback";
    if (!CopyFile(srcDir / "plant_generated.h", layout_.plant / "plant_generated.h", error))
        return false;
    if (!CopyFile(srcDir / "plant_generated.cpp", layout_.plant / "plant_generated.cpp", error))
        return false;
    return true;
#endif
}

/* --------------------------------------------------------------------------
 * 3. Stage the fixed host runtime into the work dir
 * -------------------------------------------------------------------------- */

bool SimRunner::StageHostSources(std::string& error) {
    const fs::path res = RTESIM_RESOURCE_DIR;
    struct { fs::path from; fs::path to; } files[] = {
        {res / "hostmain" / "main.cpp", layout_.host / "main.cpp"},
        {res / "shim" / "platform_shim.cpp", layout_.host / "platform_shim.cpp"},
        {res / "shim" / "shim_runtime.h", layout_.host / "shim_runtime.h"},
        {res / "shim" / "meas_access.h", layout_.host / "meas_access.h"},
        {res / "src" / "Scenario.cpp", layout_.host / "Scenario.cpp"},
        {res / "include" / "RTESim" / "Scenario.h", layout_.host / "RTESim" / "Scenario.h"},
    };
    for (const auto& item : files) {
        if (!CopyFile(item.from, item.to, error)) return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 4. Compile the scenario binary with the host system compiler
 * -------------------------------------------------------------------------- */

bool SimRunner::CompileHost(std::string& error) {
    std::vector<std::string> sources;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(layout_.sw / "generated", ec)) {
        if (entry.path().extension() == ".cpp") sources.push_back(entry.path().string());
    }
    if (sources.empty()) {
        error = "no generated controller sources in " + (layout_.sw / "generated").string();
        return false;
    }
    std::sort(sources.begin(), sources.end());
    sources.push_back((layout_.host / "main.cpp").string());
    sources.push_back((layout_.host / "platform_shim.cpp").string());
    sources.push_back((layout_.host / "Scenario.cpp").string());
    sources.push_back((layout_.plant / "plant_generated.cpp").string());

    const char* cxxEnv = std::getenv("RTESIM_CXX");
    const std::string cxx = cxxEnv && *cxxEnv ? cxxEnv : "g++";

    RTEAutomation::ProcessSpec proc;
    proc.executable = cxx;
    proc.arguments.push_back("-std=c++17");
    proc.arguments.push_back("-O2");
    proc.arguments.push_back("-w");   // vendored emitted tree stays warning-silent
    proc.arguments.push_back("-I" + (layout_.sw / "generated").string());
    proc.arguments.push_back("-I" + (layout_.sw / "Inc" / "Inverter").string());
    proc.arguments.push_back("-I" + layout_.host.string());
    proc.arguments.push_back("-I" + layout_.plant.string());
    for (const auto& src : sources) proc.arguments.push_back(src);
    proc.arguments.push_back("-o");
    proc.arguments.push_back(layout_.binary.string());

    if (logger_) logger_("compiling scenario host (" + std::to_string(sources.size()) +
                        " TUs) with " + cxx);
    std::string output;
    const auto result = RTEAutomation::RunProcess(proc, [&](const std::string& line) {
        output += line;
        output += '\n';
    });
    if (!result.started) {
        error = "could not start " + cxx + ": " + result.error;
        return false;
    }
    if (result.exitCode != 0) {
        error = "scenario host compile failed (exit " + std::to_string(result.exitCode) +
                "):\n" + output;
        return false;
    }
    if (!fs::is_regular_file(layout_.binary, ec)) {
        error = "compile succeeded but " + layout_.binary.string() + " is missing";
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 5. Run the scenario binary
 * -------------------------------------------------------------------------- */

bool SimRunner::RunHost(std::string& error) {
    std::error_code ec;
    if (!options_.outPath.parent_path().empty()) {
        fs::create_directories(options_.outPath.parent_path(), ec);
    }

    auto num = [](double x) { return RTESim::JNum(x); };

    RTEAutomation::ProcessSpec proc;
    proc.executable = layout_.binary;
    proc.arguments = {
        "--out", options_.outPath.string(),
        "--name", options_.name,
        "--duration", num(options_.durationS),
        "--telemetry-hz", num(options_.telemetryHz),
        // F15: session provenance stamp (plant flavor this build produced)
        "--plant-flavor",
#ifdef RTESIM_HAVE_PLANTCODEGEN
        "generated",
#else
        "fallback",
#endif
    };
    if (options_.dtSubS != 0.0) {   // F4: 0 = host default (200e-6)
        proc.arguments.push_back("--dt-sub");
        proc.arguments.push_back(num(options_.dtSubS));
    }
    if (!options_.dynoSeries.empty()) {
        std::ostringstream csv;
        for (std::size_t i = 0; i < options_.dynoSeries.size(); ++i) {
            if (i) csv << ',';
            csv << JNum(options_.dynoSeries[i].t) << '=' << JNum(options_.dynoSeries[i].rpm);
        }
        proc.arguments.push_back("--dyno-series");
        proc.arguments.push_back(csv.str());
    } else if (options_.dynoRpmSet) {
        proc.arguments.push_back("--dyno-rpm");
        proc.arguments.push_back(num(options_.dynoRpm));
    }
    if (options_.supply.any()) {
        std::ostringstream csv;
        bool first = true;
        auto kv = [&](const char* k, const std::string& val) {
            if (!first) csv << ',';
            first = false;
            csv << k << '=' << val;
        };
        // F03/F11: python set_supply spellings; unset keys keep the graph's
        // own DcBus values in the host (plant::PlantSupplyDefaults fill).
        if (options_.supply.hasVSet) kv("v_set", num(options_.supply.vSet));
        if (options_.supply.hasILimit) kv("i_limit", num(options_.supply.iLimit));
        if (options_.supply.hasSinkLimit) kv("sink_limit", num(options_.supply.sinkLimit));
        if (options_.supply.hasCanSink) kv("can_sink", options_.supply.canSink ? "1" : "0");
        if (options_.supply.hasCvClamp) kv("cv_clamp_v", num(options_.supply.cvClamp));
        if (options_.supply.hasCvClampSink) kv("cv_clamp_sink_a", num(options_.supply.cvClampSink));
        if (options_.supply.hasSeriesRmOhm) kv("series_r_mohm", num(options_.supply.seriesRmOhm));
        proc.arguments.push_back("--supply");
        proc.arguments.push_back(csv.str());
    }
    for (const auto& ev : options_.events) {
        std::ostringstream spec;
        spec << "t=" << num(ev.t) << " kind=" << ev.kind;
        if (!ev.arg.empty()) spec << " arg=" << ev.arg;
        proc.arguments.push_back("--event");
        proc.arguments.push_back(spec.str());
    }

    if (logger_) logger_("running scenario: " + options_.name + " (" +
                        std::to_string(options_.durationS) + " s)");
    std::string output;
    const auto result = RTEAutomation::RunProcess(proc, [&](const std::string& line) {
        output += line;
        output += '\n';
    });
    if (!result.started) {
        error = "could not start " + layout_.binary.string() + ": " + result.error;
        return false;
    }
    if (result.exitCode != 0) {
        error = "scenario host failed (exit " + std::to_string(result.exitCode) + "):\n" + output;
        return false;
    }
    if (logger_) {
        // F31: child diagnostics (e.g. unknown runtime-var warnings) must not
        // be swallowed on success; mirror warning lines into the rte log.
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("warning:", 0) == 0) logger_(line);
        }
    }
    if (!fs::is_regular_file(options_.outPath, ec)) {
        error = "scenario host did not produce " + options_.outPath.string() + "\n" + output;
        return false;
    }
    if (logger_) logger_("wrote " + options_.outPath.string());
    return true;
}

} // namespace RTESim
