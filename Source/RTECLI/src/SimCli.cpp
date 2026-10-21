/**
 * `rte sim` — headless closed-loop sw+hw simulator.
 *
 * Thin CLI over RTESim::SimRunner (Source/RTESim); see SimCli.h. v1.1 CLI
 * surface: python set_supply spellings for --supply, plant-input events for
 * controller-side plant externals, --dt-sub plumb, no --keep-work (the work
 * dir is always kept), and --format json/jsonl routed through the parent
 * CLI's Emit machinery (F12).
 */

#include "SimCli.h"

#include <RTESim/SimRunner.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

void SimUsage() {
    std::cerr <<
        "usage: rte sim --sw-graph FILE --plant-graph FILE --base-source DIR [options]\n"
        "  --templates-dir DIR       controller template dir (default: graph-embedded)\n"
        "  --plant-templates-dir DIR plant template dir (default: Assets/PlantTemplates\n"
        "                            discovered upwards from cwd / executable;\n"
        "                            env RTESIM_PLANT_TEMPLATES overrides)\n"
        "  --name NAME               scenario name (default rte_sim)\n"
        "  --duration S              simulate S seconds (default 3)\n"
        "  --dt-sub S                plant substep seconds (default 200e-6;\n"
        "                            valid 1e-6..1e-3; use 50e-6 for the stiff IM rig)\n"
        "  --dyno-rpm R              hold dyno at R mechanical rpm\n"
        "                            (mutually exclusive with --dyno-series)\n"
        "  --dyno-series 't=rpm,t=rpm,...'  piecewise-constant dyno schedule\n"
        "  --supply 'k=v,..'         bench supply overrides; keys (python\n"
        "                            set_supply spellings): v_set,i_limit,\n"
        "                            sink_limit,can_sink,cv_clamp_v,\n"
        "                            cv_clamp_sink_a,series_r_mohm; unset keys\n"
        "                            keep the plant graph's DcBus values\n"
        "  --event 't=.. kind=.. arg=..'    repeatable; kinds:\n"
        "                            reset, start, stop,\n"
        "                            var-set (arg=Name=Value),\n"
        "                            plant-input (arg=Name=Value) — THE mechanism\n"
        "                              for InductDq.Omega_Cmd (V/Hz rigs); a ramp\n"
        "                              is a series of one-shot plant-input events,\n"
        "                            fault-inject (arg=OpenGates|EncoderDisconnect|\n"
        "                              SensorRail), fault-clear (arg=Name|empty=all),\n"
        "                            dyno-set (arg=rpm), gate-ready (arg=0|1; 0\n"
        "                              forces idle physics + latches the gate fault)\n"
        "  --telemetry-hz HZ         telemetry publish rate (default 100)\n"
        "  --out PATH                session output (.jsonl)\n"
        "  --workdir DIR             staging dir (default <out>.work, always kept)\n"
        "  --allow-fallback-plant    opt into the built-in Gen7-bench fallback\n"
        "                            plant when this build has no PlantCodegen\n"
        "                            (default: refuse to run — the fallback is a\n"
        "                            fixed physics substitute, not the graph)\n"
        "\n"
        "env: RTESIM_CXX selects the compiler for the scenario host (default g++).\n"
        "     --format json|jsonl on the parent CLI streams progress events and a\n"
        "     final artifact event with the session path.\n";
}

} // namespace

int RunSimCommand(const std::vector<std::string>& args,
                  const std::function<void(const nlohmann::json&)>& emit) {
    RTESim::SimOptions options;
    auto tell = [&](const nlohmann::json& value) {
        if (emit) emit(value);
    };

    std::string error;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto nextValue = [&](const char* name, std::string& target) {
            if (i + 1 >= args.size()) {
                error = std::string("missing value for ") + name;
                return false;
            }
            target = args[++i];
            return true;
        };
        std::string value;
        if (arg == "--sw-graph") { if (!nextValue("--sw-graph", value)) break; options.swGraphPath = value; }
        else if (arg == "--plant-graph") { if (!nextValue("--plant-graph", value)) break; options.plantGraphPath = value; }
        else if (arg == "--base-source") { if (!nextValue("--base-source", value)) break; options.baseSourceDir = value; }
        else if (arg == "--templates-dir") { if (!nextValue("--templates-dir", value)) break; options.templatesDir = value; }
        else if (arg == "--plant-templates-dir") { if (!nextValue("--plant-templates-dir", value)) break; options.plantTemplatesDir = value; }
        else if (arg == "--workdir") { if (!nextValue("--workdir", value)) break; options.workDir = value; }
        else if (arg == "--out") { if (!nextValue("--out", value)) break; options.outPath = value; }
        else if (arg == "--name") { if (!nextValue("--name", value)) break; options.name = value; }
        else if (arg == "--duration") {
            if (!nextValue("--duration", value)) break;
            // F21: same checked parse (isfinite) as the scenario layer
            if (!RTESim::ParseDouble(value, options.durationS) || options.durationS <= 0.0) {
                error = "invalid --duration: " + value;
                break;
            }
        }
        else if (arg == "--telemetry-hz") {
            if (!nextValue("--telemetry-hz", value)) break;
            if (!RTESim::ParseDouble(value, options.telemetryHz) || options.telemetryHz <= 0.0) {
                error = "invalid --telemetry-hz: " + value;
                break;
            }
        }
        else if (arg == "--dt-sub") {
            // F4: plant substep plumb; stiff IM rigs want 50e-6.
            if (!nextValue("--dt-sub", value)) break;
            double dt = 0.0;
            if (!RTESim::ParseDouble(value, dt) || dt < 1e-6 || dt > 1e-3) {
                error = "invalid --dt-sub (need 1e-6..1e-3 finite): " + value;
                break;
            }
            options.dtSubS = dt;
        }
        else if (arg == "--dyno-rpm") {
            if (!nextValue("--dyno-rpm", value)) break;
            if (!RTESim::ParseDouble(value, options.dynoRpm)) {
                error = "invalid --dyno-rpm: " + value;
                break;
            }
            options.dynoRpmSet = true;
        }
        else if (arg == "--dyno-series") {
            if (!nextValue("--dyno-series", value)) break;
            if (!RTESim::ParseDynoSeries(value, options.dynoSeries, error)) {
                error = "--dyno-series: " + error;
                break;
            }
        }
        else if (arg == "--supply") {
            if (!nextValue("--supply", value)) break;
            if (!RTESim::ParseSupplySpec(value, options.supply, error)) {
                error = "--supply: " + error;
                break;
            }
        }
        else if (arg == "--event") {
            if (!nextValue("--event", value)) break;
            RTESim::ScenarioEvent ev;
            if (!RTESim::ParseEventSpec(value, ev, error)) {
                error = "--event: " + error;
                break;
            }
            options.events.push_back(ev);
        }
        else if (arg == "--allow-fallback-plant") options.allowFallbackPlant = true;
        else if (arg == "--help" || arg == "-h") { SimUsage(); return 0; }  // F34: help is a request
        else { error = "unknown option: " + arg; break; }
    }

    if (error.empty() && options.dynoRpmSet && !options.dynoSeries.empty()) {
        // F28: same knob spelled twice would silently prefer one
        error = "--dyno-rpm and --dyno-series are mutually exclusive";
    }
    if (error.empty()) {
        if (options.swGraphPath.empty()) error = "--sw-graph is required";
        else if (options.plantGraphPath.empty()) error = "--plant-graph is required";
        else if (options.baseSourceDir.empty()) error = "--base-source is required";
        else if (options.outPath.empty()) error = "--out is required";
    }
    if (!error.empty()) {
        tell({{"event","error"},{"message","rte sim: " + error}});
        if (!emit) std::cerr << "rte sim: " << error << '\n';
        SimUsage();
        return 2;
    }

    // F12: progress routes through the parent's Emit machinery when given
    // (json/jsonl), else to stderr in text mode (unchanged behavior).
    const fs::path outPath = options.outPath;   // moved into the runner below
    RTESim::SimRunner runner(std::move(options), [&](const std::string& line) {
        if (emit) {
            tell({{"event","progress"},{"phase","sim"},{"message",line}});
        } else {
            std::cerr << "rte sim: " << line << '\n';
        }
    });
    std::string runError;
    if (!runner.Run(runError)) {
        tell({{"event","error"},{"message","rte sim: " + runError}});
        if (!emit) std::cerr << "rte sim: " << runError << '\n';
        return 4;
    }
    std::error_code ec;
    fs::path absOut = fs::weakly_canonical(fs::absolute(outPath, ec), ec);
    if (ec || absOut.empty()) absOut = fs::absolute(outPath);
    tell({{"event","artifact"},{"kind","session-jsonl"},{"path",absOut.string()}});
    tell({{"event","complete"},{"success",true}});
    return 0;
}
