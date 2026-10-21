/**
 * rte-sim scenario host (compiled per-invocation by SimRunner).
 *
 * Runs the ACTUAL emitted controller firmware domains (adc_isr/tim_isr
 * @2.5 kHz, app_loop + vsense @100 Hz) against the generated (or fallback)
 * plant through the PlantCodegen contract, and writes the v3
 * rte-runtime-session-jsonl schema of twin/harness.py's TwinSession.
 *
 * Scheduler + record vocabulary are a faithful port of the proven seed
 * twin/native/src/main.cpp (constants cited inline); the plant access goes
 * through plant::PlantSetDuty/PlantStep/... instead of twin::PlantPmsm
 * fields, and the events timeline is argv-driven instead of hardcoded.
 *
 * ---------------------------------------------------------------------------
 * F02 — REAL-protection status (review round 2026-10-20, verdict: the real
 * controller protection chain CANNOT trip in this host today — do NOT paper
 * it over with a simulated latch):
 *  - The base image ships the real chain (Images/Gen6FW/Src/Inverter/
 *    Control/{FaultManager,ControlSupervisor,FocControlManager}.cpp + the
 *    software OC monitor at Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp
 *    m_oc_threshold_a, default 500 A, OC_CONSEC_SAMPLES=3), but SimRunner
 *    compiles ONLY sw/generated/*.cpp + this host + shim + plant: none of
 *    those units is linked, and the emitted domains contain zero references
 *    to FaultManager/ControlSupervisor (grep-verified in a produced workdir).
 *  - Missing platform signals/FRAM keys the real chain would need:
 *      1. RteParamStore FRAM backing (CY15B102Q over SPI4) — the shim only
 *         implements the platform_config_* API used by graph Config nodes;
 *         monitor keys Hw.PhCur.RefMinV/MaxV, Hw.Temp.Bx.*, Hw.DcLink.*,
 *         Hw.OcSw.A, Can.AWD.SetA are never seeded or read in this build.
 *      2. ADC analog-watchdog IRQ path (AdcWatchdog source) + injected-rank
 *         raw frames beyond the shim's two-rank burst model.
 *      3. TIM1 hardware break (DESAT -> PwmBreak) and gate-driver /RDY,
 *         UVLO pins (GateDriverUvlo) GPIO/EXTI semantics.
 *      4. MAX22530 SPI device model (Ov/Uv/Adc/Comm/Field sources).
 *      5. Temp-sensor front-end open/short detection (TempSensor) and
 *         encoder DMA fault (EncoderDma) hooks.
 *  - Evidence collected 2026-10-20: a 24 s IqVar=2000 @ 500 rpm scenario
 *    sustained plant-side currents > 500 A for 69 telemetry frames (peak
 *    571.5 A) while the session kept pwm_moe=1 / control_state RUNNING /
 *    fault_flags_hex = 0x00000000 - zero trip vocabulary. The next step is
 *    picking items off the TODO list above (FRAM store first).
 * --------------------------------------------------------------------------- */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "RTESim/Scenario.h"

#include "meas_access.h"
#include "plant_generated.h"
#include "shim_runtime.h"
#include "platform_api.h"

#include "domain_adc_isr_generated.h"
#include "domain_app_loop_generated.h"
#include "domain_tim_isr_generated.h"
#include "domain_vsense_generated.h"

// FRAM config seed, generated next to this file by RTESim::SimRunner at
// staging (sim_config_seed.inc): kGraphConfigSeed[] = twin baseline table
// overlaid with the sw graph's Values.Config defaults.
#include "sim_config_seed.inc"

RTESIM_MEAS_ACCESSOR(Iu_A)
RTESIM_MEAS_ACCESSOR(Iv_A)
RTESIM_MEAS_ACCESSOR(Iw_A)
RTESIM_MEAS_ACCESSOR(Vdc_V)
RTESIM_MEAS_ACCESSOR(Enc_Sin)
RTESIM_MEAS_ACCESSOR(Enc_Cos)
RTESIM_MEAS_ACCESSOR(Temp_Inv1_C)
RTESIM_MEAS_ACCESSOR(Temp_Inv2_C)
RTESIM_MEAS_ACCESSOR(Temp_Inv3_C)
RTESIM_MEAS_ACCESSOR(Temp_Motor_C)
RTESIM_MEAS_ACCESSOR(Mech_Rpm)
RTESIM_MEAS_ACCESSOR(Elec_Rpm)
RTESIM_MEAS_ACCESSOR(Dclink_I_A)
RTESIM_MEAS_ACCESSOR(Dclink_P_W)
RTESIM_MEAS_ACCESSOR(Vu_V)
RTESIM_MEAS_ACCESSOR(Vv_V)
RTESIM_MEAS_ACCESSOR(Vw_V)
RTESIM_MEAS_ACCESSOR(P_Heat_W)
RTESIM_MEAS_ACCESSOR(P_Dc_W)
RTESIM_MEAS_ACCESSOR(P_ShaftEm_W)

namespace {

using RTESim::DynoStep;
using RTESim::ScenarioEvent;
using RTESim::SupplyOverride;

constexpr double kDt = 400e-6;   // control period (tim_isr 2.5 kHz)

/* --------------------------------------------------------------------------
 * v3 session JSONL writer — byte-compatible port of twin/native's
 * SessionWriter (same field order, same %.9g formatting) with the python
 * TwinSession n_records accounting: EVERY record bumps it, state included.
 * -------------------------------------------------------------------------- */

struct SessionWriter {
    FILE* f = nullptr;
    long n_records = 0;

    void console(double t, const std::string& text) {
        std::fprintf(f, "{\"type\":\"console\",\"t\":%s,\"sequence\":%ld,\"text\":\"%s\"}\n",
                     RTESim::JNum(RTESim::Rt6(t)).c_str(), n_records,
                     RTESim::JStr(text).c_str());
        ++n_records;
    }

    void command(double t, const std::string& text) {
        std::fprintf(f,
                     "{\"type\":\"command\",\"t\":%s,\"received_t\":%s,\"source\":\"twin\","
                     "\"text\":\"%s\",\"sent\":true}\n",
                     RTESim::JNum(RTESim::Rt6(t)).c_str(),
                     RTESim::JNum(RTESim::Rt6(t)).c_str(),
                     RTESim::JStr(text).c_str());
        ++n_records;
    }

    void state(double t, const char* signal, const std::string& jsonValue) {
        ++n_records;   // python TwinSession counts every record
        std::fprintf(f, "{\"type\":\"state\",\"t\":%s,\"signal\":\"%s\",\"value\":%s}\n",
                     RTESim::JNum(RTESim::Rt6(t)).c_str(), signal, jsonValue.c_str());
    }

    void telemetry(double t, const std::vector<std::pair<std::string, std::string>>& kvs) {
        std::fprintf(f, "{\"type\":\"telemetry\",\"t\":%s,\"values\":{",
                     RTESim::JNum(RTESim::Rt6(t)).c_str());
        bool first = true;
        for (const auto& kv : kvs) {
            if (!first) std::fputc(',', f);
            first = false;
            std::fprintf(f, "\"%s\":%s", kv.first.c_str(), kv.second.c_str());
        }
        std::fputs("}}\n", f);
        ++n_records;
    }

    bool open(const char* path, const std::string& scenario, const std::string& plantFlavor) {
        f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f, "%s\n",
                     RTESim::FormatSessionStartLine(scenario, "rte-sim", plantFlavor).c_str());
        return true;
    }

    void finish() {
        std::fprintf(f, "{\"type\":\"session_end\",\"final_stats\":{\"n_records\":%ld},"
                     "\"signal_summaries\":{}}\n", n_records);
        std::fclose(f);
    }
};

/* --------------------------------------------------------------------------
 * Scenario args
 * -------------------------------------------------------------------------- */

struct HostArgs {
    std::string outPath;
    std::string name = "rte_sim";
    double duration = 3.0;
    double telemetryHz = 100.0;
    double dtSub = 200e-6;             // monolith plant substep (twin/native dt_sub)
    double dynoRpm = 0.0;
    bool dynoRpmSet = false;           // F28: "asked 0" vs "not asked" (Rpm0 kept)
    std::string plantFlavor = "generated";  // F15 provenance stamp
    std::vector<DynoStep> dynoSeries;
    SupplyOverride supply;
    std::vector<ScenarioEvent> events;
};

void Usage() {
    std::fprintf(stderr,
        "usage: sim_host --out PATH [options]\n"
        "  --name NAME              scenario name (default rte_sim)\n"
        "  --duration S             simulate S seconds (default 3)\n"
        "  --telemetry-hz HZ        telemetry publish rate (default 100)\n"
        "  --dt-sub S               plant substep, 1e-6..1e-3 s (default 0.0002)\n"
        "  --dyno-rpm R             hold dyno at R mechanical rpm (xor --dyno-series)\n"
        "  --dyno-series 't=rpm,t=rpm,...'   piecewise-constant dyno schedule\n"
        "  --supply 'k=v,...'       keys: v_set,i_limit,sink_limit,can_sink,\n"
        "                           cv_clamp_v,cv_clamp_sink_a,series_r_mohm;\n"
        "                           unset keys keep the graph's DcBus values\n"
        "  --plant-flavor F         generated|fallback (session provenance)\n"
        "  --event 't=.. kind=.. arg=..'     repeatable; kinds: reset, start, stop,\n"
        "                           var-set (arg=Name=Value),\n"
        "                           plant-input (arg=Name=Value, e.g. InductDq\n"
        "                           Omega_Cmd for the V/Hz rigs),\n"
        "                           fault-inject (arg=OpenGates|EncoderDisconnect|\n"
        "                           SensorRail), fault-clear (arg=Name|empty=all),\n"
        "                           dyno-set (arg=rpm), gate-ready (arg=0|1)\n");
}

bool ParseArgs(int argc, char** argv, HostArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what, std::string& dst) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                return false;
            }
            dst = argv[++i];
            return true;
        };
        // F21: checked strtod (isfinite) — same parser the CLI layer uses.
        auto nextNum = [&](const char* what, double& dst) {
            std::string v;
            if (!next(what, v)) return false;
            if (!RTESim::ParseDouble(v, dst)) {
                std::fprintf(stderr, "%s: not a finite number: %s\n", what, v.c_str());
                return false;
            }
            return true;
        };
        std::string v;
        if (a == "--out") { if (!next("--out", args.outPath)) return false; }
        else if (a == "--name") { if (!next("--name", args.name)) return false; }
        else if (a == "--plant-flavor") { if (!next("--plant-flavor", args.plantFlavor)) return false; }
        else if (a == "--duration") { if (!nextNum("--duration", args.duration)) return false; }
        else if (a == "--telemetry-hz") { if (!nextNum("--telemetry-hz", args.telemetryHz)) return false; }
        else if (a == "--dt-sub") { if (!nextNum("--dt-sub", args.dtSub)) return false; }
        else if (a == "--dyno-rpm") { if (!nextNum("--dyno-rpm", args.dynoRpm)) return false; args.dynoRpmSet = true; }
        else if (a == "--dyno-series") {
            if (!next("--dyno-series", v)) return false;
            std::string err;
            if (!RTESim::ParseDynoSeries(v, args.dynoSeries, err)) {
                std::fprintf(stderr, "--dyno-series: %s\n", err.c_str());
                return false;
            }
        }
        else if (a == "--supply") {
            if (!next("--supply", v)) return false;
            std::string err;
            if (!RTESim::ParseSupplySpec(v, args.supply, err)) {
                std::fprintf(stderr, "--supply: %s\n", err.c_str());
                return false;
            }
        }
        else if (a == "--event") {
            if (!next("--event", v)) return false;
            ScenarioEvent ev;
            std::string err;
            if (!RTESim::ParseEventSpec(v, ev, err)) {
                std::fprintf(stderr, "--event: %s\n", err.c_str());
                return false;
            }
            args.events.push_back(ev);
        }
        else if (a == "--help" || a == "-h") { Usage(); std::exit(0); }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return false; }
    }
    if (args.outPath.empty()) {
        std::fprintf(stderr, "--out is required\n");
        return false;
    }
    // F4: dt-sub window (validated at the CLI first; the host re-checks).
    if (args.plantFlavor != "generated" && args.plantFlavor != "fallback") {
        std::fprintf(stderr, "--plant-flavor must be generated|fallback\n");
        return false;
    }
    if (args.dynoRpmSet && !args.dynoSeries.empty()) {
        std::fprintf(stderr, "--dyno-rpm and --dyno-series are mutually exclusive\n");
        return false;
    }
    if (args.duration <= 0.0 || args.dtSub < 1e-6 || args.dtSub > 1e-3 ||
        args.telemetryHz <= 0.0) {
        std::fprintf(stderr, "invalid duration/dt-sub/telemetry-hz\n");
        return false;
    }
    std::stable_sort(args.events.begin(), args.events.end(),
                     [](const ScenarioEvent& a, const ScenarioEvent& b) { return a.t < b.t; });
    std::stable_sort(args.dynoSeries.begin(), args.dynoSeries.end(),
                     [](const DynoStep& a, const DynoStep& b) { return a.t < b.t; });
    return true;
}

/* --------------------------------------------------------------------------
 * Runtime-var access through the generated g_<domain>_vars tables
 * (firmware runtime-var mechanism; native used g_tim_isr_vars only, we walk
 * all four emitted domains).
 * -------------------------------------------------------------------------- */

void PlantSetAllFaults(plant::PlantState& plant, bool active) {
    plant::PlantSetFault(plant, "OpenGates", active);
    plant::PlantSetFault(plant, "EncoderDisconnect", active);
    plant::PlantSetFault(plant, "SensorRail", active);
}

} // namespace

int RunScenario(int argc, char** argv) {
    HostArgs args;
    if (!ParseArgs(argc, argv, args)) { Usage(); return 2; }

    // plant boot (generated plant or staged fallback; identical API)
    plant::PlantState plantState;
    plant::PlantInit(plantState);
    {
        // F03/F11: fill from the plant's graduated supply defaults and let
        // only user-given --supply keys override (no hardcoded bench fill —
        // a non-gen7 graph keeps its own DcBus values).
        double vSet = 0.0, iLimit = 0.0, sinkLimit = 0.0, cvClamp = 0.0,
               cvClampSink = 0.0, seriesRmOhm = 0.0;
        bool canSink = false;
        plant::PlantSupplyDefaults(plantState, &vSet, &iLimit, &sinkLimit,
                                   &canSink, &cvClamp, &cvClampSink, &seriesRmOhm);
        if (args.supply.any()) {
            const SupplyOverride& s = args.supply;
            plant::PlantSetSupply(plantState,
                s.hasVSet ? s.vSet : vSet,
                s.hasILimit ? s.iLimit : iLimit,
                s.hasSinkLimit ? s.sinkLimit : sinkLimit,
                s.hasCanSink ? s.canSink : canSink,
                s.hasCvClamp ? s.cvClamp : cvClamp,
                s.hasCvClampSink ? s.cvClampSink : cvClampSink,
                s.hasSeriesRmOhm ? s.seriesRmOhm : seriesRmOhm);
        }
    }
    // F28: with no --dyno-rpm and no series the graph's Dyno.Rpm0 stands.
    if (args.dynoRpmSet) {
        plant::PlantSetDynoRpm(plantState, static_cast<float>(args.dynoRpm));
    }

    plant::PlantMeas meas{};
    plant::PlantCollectMeas(plantState, meas);

    rtesim_host::ShimRuntime& shim = rtesim_host::ShimRuntime::instance();
    shim.plant = &plantState;
    shim.meas = &meas;
    // FRAM seed: derived from the sw graph the user actually runs
    // (Values.Config Key/DefaultValue, merged over the twin/native
    // baseline by SimRunner at staging — see sim_config_seed.inc), so both
    // engines run identical control law inputs AND GUI tuning of a config
    // default changes the sim. Baseline keys keep the python replica's
    // FocController.DEFAULTS values when the graph is silent on them.
    for (const auto& seed : kGraphConfigSeed) shim.config[seed.first] = seed.second;

    SessionWriter sess;
    if (!sess.open(args.outPath.c_str(), args.name, args.plantFlavor)) {
        std::fprintf(stderr, "cannot open %s\n", args.outPath.c_str());
        return 4;
    }

    // boot: generated domain init (config nodes load the FRAM seed here)
    app::AdcIsrState adc{};
    app::TimIsrState tim{};
    app::AppLoopState alp{};
    app::VsenseState vse{};
    app::AdcIsrInit(adc);
    app::TimIsrInit(tim);
    app::AppLoopInit(alp);
    app::VsenseInit(vse);

    // boot log preamble (verbatim twin/harness.py text via twin/native)
    sess.console(0.0, "[FOC] Kp set to 0.0300");
    sess.console(0.0, "[FOC] Ki set to 10.0000");
    {
        // F07: read the plant's own boot params instead of gen7 constants;
        // any NaN (e.g. encoder-less rig w/o OffsetDeg) keeps the legacy
        // gen7-monolith line verbatim (F7-fallback behavior).
        double bootPoles = 0.0, bootRs = 0.0, bootOff = 0.0;
        plant::PlantBootParams(plantState, &bootPoles, &bootRs, &bootOff);
        char buf[256];
        if (std::isfinite(bootPoles) && std::isfinite(bootRs) && std::isfinite(bootOff)) {
            std::snprintf(buf, sizeof buf,
                          "[CFG] motor config loaded from FRAM: type=pmsm_ipm poles=%.0f "
                          "offset=%.3f deg sign=1 R=%.4f ohm kp=0.0300 ki=10.0000",
                          bootPoles * 2.0, bootOff, bootRs);
            /* graph Poles convention is pole PAIRS (node.json); the recorded
             * boot vocabulary prints physical poles (Motor.Poles = 10 on gen7) */
        } else {
            std::snprintf(buf, sizeof buf,
                          "[CFG] motor config loaded from FRAM: type=pmsm_ipm poles=10 "
                          "offset=33.034 deg sign=1 R=0.0076 ohm kp=0.0300 ki=10.0000");
        }
        sess.console(0.0, buf);
    }
    sess.console(0.0, "[SHELL] Command shell ready; type HELP for list");
    sess.console(0.0, "[CUR] start cal done U=-4.565 V=-2.482");

    bool gate_fault_latched = true;   // boot latch like hardware
    bool started = false;             // ControlSupervisor RUNNING latch
    bool gateReady = true;            // F36: gate-ready flag (telemetry + physics)
    // F16: OpenGates has two independent raisers — the python harness's idle
    // physics (pwm_gate_off injected whenever the supervisor is not RUNNING;
    // gate-ready=0 forces the same per F36) and the user's fault-inject
    // events. Track them separately so start/stop transitions never clobber
    // an event-set fault the user didn't clear.
    bool userOpenGates = false;
    bool appliedOpenGates = false;    // last value written to the plant

    auto reconcileOpenGates = [&]() {
        const bool desired = userOpenGates || !(started && gateReady);
        if (desired != appliedOpenGates) {
            plant::PlantSetFault(plantState, "OpenGates", desired);
            appliedOpenGates = desired;
        }
    };
    reconcileOpenGates();   // boot-idle: gates open before the first substep

    std::size_t ev_i = 0;
    std::size_t dyno_i = 0;

    // F23: half-to-even quantization of the cadence ratios (python round()).
    const int nSub = std::max(1, static_cast<int>(std::nearbyint(kDt / args.dtSub)));
    const long publishEvery =
        std::max(1L, static_cast<long>(std::nearbyint(1.0 / (args.telemetryHz * kDt))));

    const long n_steps = static_cast<long>(args.duration / kDt);
    for (long k = 0; k < n_steps; ++k) {
        const double t = k * kDt;
        shim.now_us = static_cast<unsigned long long>(t * 1e6 + 0.5);

        // ---- dyno speed series FIRST (F24: matches the python harness;
        // events at the same t must see the current dyno state) ----
        while (dyno_i < args.dynoSeries.size() && t >= args.dynoSeries[dyno_i].t - 1e-12) {
            plant::PlantSetDynoRpm(plantState, static_cast<float>(args.dynoSeries[dyno_i].rpm));
            ++dyno_i;
        }

        // ---- scripted events ----
        while (ev_i < args.events.size() && t >= args.events[ev_i].t - 1e-12) {
            const ScenarioEvent& ev = args.events[ev_i++];
            if (ev.kind == "reset") {
                sess.command(t, "fault reset");
                sess.console(t, "[FAULT] gate reset: ready=Y fault=Y MOE=0 outputs_restored=reset");
                sess.console(t, "[FAULT] MAX22530 reset: interrupt=FAILED filter=FAILED");
                sess.console(t, "[FAULT] clear 'all' complete; active=0x00000000 supervisor=IDLE");
                sess.console(t, "[FAULT] no faults active; persistent conditions may reassert");
                sess.console(t, "[FAULT] none active");
                gate_fault_latched = false;
                PlantSetAllFaults(plantState, false);
                userOpenGates = false;
                appliedOpenGates = false;   // re-sync with the cleared plant
            } else if (ev.kind == "start") {
                sess.command(t, "control start");
                if (gate_fault_latched) {
                    sess.console(t, "[SUP] ERROR: gate driver fault latched");
                    sess.console(t, "[SHELL] control start failed: MAIN motor control (RTE graph)");
                } else {
                    // ControlSupervisor start path: reset domain state, enable PWM
                    app::AdcIsrStart(adc);
                    app::TimIsrStart(tim);
                    app::AppLoopStart(alp);
                    app::VsenseStart(vse);
                    started = true;   // F16: clears the host's idle OpenGates
                    sess.console(t, "[SUP] STARTED f_sw=2500 Hz f_u=5000 Hz");
                    sess.console(t, "[SHELL] control started: MAIN motor control (RTE graph)");
                }
            } else if (ev.kind == "stop") {
                // F36: mirror of the start path (python prot.control_stop()).
                // Second `start` re-runs the domain reset above.
                sess.command(t, "control stop");
                started = false;
                sess.console(t, "[SHELL] control stopped");
            } else if (ev.kind == "gate-ready") {
                // F36: twin harness gate_ready vocabulary. 0 forces idle
                // physics (F16) + latches the gate fault; 1 just re-readies
                // (the latch still needs an explicit reset), silently in the
                // session like the python card kind.
                gateReady = ev.arg == "1";
                if (!gateReady) gate_fault_latched = true;
            } else if (ev.kind == "var-set") {
                const std::size_t eq = ev.arg.find('=');
                const std::string varName = ev.arg.substr(0, eq);
                double parsed = 0.0;
                RTESim::ParseDouble(ev.arg.substr(eq + 1), parsed);
                const float value = static_cast<float>(parsed);
                char cmd[96], echo[128];
                std::snprintf(cmd, sizeof cmd, "var set %s %g", varName.c_str(), value);
                std::snprintf(echo, sizeof echo, "[SHELL] %s = %.4f", varName.c_str(), value);
                sess.command(t, cmd);
                bool found = false;
                struct VarTable { const RteParamDesc* table; size_t count; void* state; };
                const VarTable tables[] = {
                    {app::g_adc_isr_vars, app::g_adc_isr_var_count, &adc},
                    {app::g_tim_isr_vars, app::g_tim_isr_var_count, &tim},
                    {app::g_app_loop_vars, app::g_app_loop_var_count, &alp},
                    {app::g_vsense_vars, app::g_vsense_var_count, &vse},
                };
                for (const auto& tb : tables) {
                    for (size_t i = 0; i < tb.count; ++i) {
                        if (tb.table[i].name && !std::strcmp(tb.table[i].name, varName.c_str())) {
                            tb.table[i].set(tb.state, value);
                            found = true;
                        }
                    }
                }
                // F31: no [SHELL] echo for a write that never happened; the
                // warning reaches the rte log via SimRunner's mirror.
                if (!found) {
                    std::fprintf(stderr, "warning: unknown runtime var '%s'\n", varName.c_str());
                } else {
                    sess.console(t, echo);
                }
            } else if (ev.kind == "plant-input") {
                // F13/F7: THE scenario mechanism for controller-side external
                // plant inputs (InductDq.Omega_Cmd on the V/Hz rigs). One-shot
                // plant::PlantSetInput at this t; a ramp is a series of these.
                const std::size_t eq = ev.arg.find('=');
                const std::string portName = ev.arg.substr(0, eq);
                double parsed = 0.0;
                RTESim::ParseDouble(ev.arg.substr(eq + 1), parsed);
                char cmd[128], line[160];
                std::snprintf(cmd, sizeof cmd, "plant input %s %g", portName.c_str(), parsed);
                sess.command(t, cmd);
                if (!plant::PlantSetInput(plantState, portName.c_str(),
                                          static_cast<float>(parsed))) {
                    std::fprintf(stderr,
                                 "plant-input: no unwired plant input port named '%s'\n",
                                 portName.c_str());
                    sess.finish();
                    return 4;
                }
                std::snprintf(line, sizeof line, "[PLANT] input %s = %.4f",
                              portName.c_str(), parsed);
                sess.console(t, line);
            } else if (ev.kind == "fault-inject") {
                char cmd[128];
                std::snprintf(cmd, sizeof cmd, "fault inject %s", ev.arg.c_str());
                sess.command(t, cmd);
                if (ev.arg == "OpenGates") {
                    userOpenGates = true;   // F16: OR-managed, see reconcile
                } else if (!plant::PlantSetFault(plantState, ev.arg.c_str(), true)) {
                    // v1.1 contract: bool-checked (parse already gates names
                    // per F01; this is the belt for graphs lacking a port)
                    std::fprintf(stderr, "fault-inject: plant graph has no fault '%s'\n",
                                 ev.arg.c_str());
                    sess.finish();
                    return 4;
                }
                char line[160];
                std::snprintf(line, sizeof line, "[PLANT] fault injected: %s", ev.arg.c_str());
                sess.console(t, line);
            } else if (ev.kind == "fault-clear") {
                const std::string which = ev.arg.empty() ? "all" : ev.arg;
                char cmd[128];
                std::snprintf(cmd, sizeof cmd, "fault clear %s", which.c_str());
                sess.command(t, cmd);
                if (ev.arg.empty()) {
                    PlantSetAllFaults(plantState, false);
                    userOpenGates = false;
                    appliedOpenGates = false;   // re-sync (idle re-asserts)
                } else if (ev.arg == "OpenGates") {
                    userOpenGates = false;
                } else {
                    const bool known =
                        plant::PlantSetFault(plantState, ev.arg.c_str(), false);
                    if (!known) {
                        char note[160];
                        std::snprintf(note, sizeof note,
                                      "[PLANT] warning: unknown fault name '%s' (no-op)",
                                      ev.arg.c_str());
                        sess.console(t, note);
                    }
                }
                char line[160];
                std::snprintf(line, sizeof line, "[PLANT] fault cleared: %s", which.c_str());
                sess.console(t, line);
            } else if (ev.kind == "dyno-set") {
                double rpm = 0.0;
                RTESim::ParseDouble(ev.arg, rpm);
                char cmd[96], line[128];
                std::snprintf(cmd, sizeof cmd, "dyno set %g", rpm);
                std::snprintf(line, sizeof line, "[PLANT] dyno speed set to %g rpm", rpm);
                sess.command(t, cmd);
                plant::PlantSetDynoRpm(plantState, static_cast<float>(rpm));
                sess.console(t, line);
            }
        }

        // F16: idle physics — open gates whenever not driving (events above
        // have settled; the OR keeps user-injected faults untouched).
        reconcileOpenGates();

        const bool driving = started && gateReady;

        // ---- 2.5 kHz domains: adc burst sampling always live; control gated ----
        // (python harness: the controller keeps stepping while the
        // supervisor is RUNNING; gate_ready pins the outputs instead)
        platform_set_current_domain_dt(static_cast<float>(kDt));
        app::AdcIsrStep(adc);
        if (started) {
            platform_set_current_domain_dt(static_cast<float>(kDt));
            app::TimIsrStep(tim);
        }

        // ---- plant: freeze the duty vector for the control period, substep ----
        // python harness publishes the PUSHED duty vector (50/50/50 whenever
        // outputs are inhibited), so keep the pushed values for telemetry.
        float duty_u = 50.0f, duty_v = 50.0f, duty_w = 50.0f;
        if (driving) {
            duty_u = shim.pwm_du; duty_v = shim.pwm_dv; duty_w = shim.pwm_dw;
        }
        plant::PlantSetDuty(plantState, duty_u, duty_v, duty_w);
        for (int s = 0; s < nSub; ++s) plant::PlantStep(plantState, args.dtSub);
        plant::PlantCollectMeas(plantState, meas);

        // ---- 100 Hz app domains (native cadence: every 25 control periods) ----
        if (k % 25 == 0) {
            app::AppLoopStep(alp);
            app::VsenseStep(vse);
        }

        // ---- telemetry publish at the configured rate ----
        if (k % publishEvery == 0) {
            std::vector<std::pair<std::string, std::string>> v;
            v.reserve(64);
            auto tel = [&](const char* key, double fallback) {
                auto it = shim.telemetry.find(key);
                v.emplace_back(key, RTESim::JNum(it != shim.telemetry.end() ? it->second : fallback));
            };
            auto num = [&](const char* key, double val) { v.emplace_back(key, RTESim::JNum(val)); };
            auto nullv = [&](const char* key) { v.emplace_back(key, "null"); };
            auto getm = [&](double (*acc)(const plant::PlantMeas&, double), double fallback) {
                return acc(meas, fallback);
            };

            num("cg_du", duty_u);
            num("cg_dv", duty_v);
            num("cg_dw", duty_w);
            if (driving) {
                tel("cg_id_a", 0.0);
                tel("cg_iq_a", 0.0);
            } else {
                num("cg_id_a", 0.0);
                num("cg_iq_a", 0.0);
            }
            tel("cg_vd_v", 0.0);
            tel("cg_vq_v", 0.0);
            tel("cg_theta_rad", 0.0);
            num("cg_vdc_v", getm(Meas_Vdc_V, 0.0));
            num("vdc_v", getm(Meas_Vdc_V, 0.0));
            num("dclink_i_a", getm(Meas_Dclink_I_A, 0.0));
            num("dclink_p_w", getm(Meas_Dclink_P_W, 0.0));
            tel("cg_iu_a", getm(Meas_Iu_A, 0.0));
            tel("cg_iv_a", getm(Meas_Iv_A, 0.0));
            tel("cg_iw_a", getm(Meas_Iw_A, 0.0));
            // graphs that don't telemetry-log speeds (the V/Hz rigs) fall
            // back to the plant meas channels, mirroring the python harness
            // publishing the controller-read speeds unconditionally.
            tel("Mech_RPM", getm(Meas_Mech_Rpm, 0.0));
            tel("Elec_RPM", getm(Meas_Elec_Rpm, 0.0));
            tel("EncoderSin", getm(Meas_Enc_Sin, 0.0));
            tel("EncoderCos", getm(Meas_Enc_Cos, 0.0));
            tel("GapTicks", 0.0);
            num("hz_adc_isr", 2500);
            num("hz_tim_isr", 2500);
            num("hz_app_loop", 3697);
            num("hz_vsense", 3697);
            tel("temp_inv1_c", getm(Meas_Temp_Inv1_C, 0.0));
            tel("temp_inv2_c", getm(Meas_Temp_Inv2_C, 0.0));
            tel("temp_inv3_c", getm(Meas_Temp_Inv3_C, std::nan("")));
            tel("temp_motor_c", getm(Meas_Temp_Motor_C, std::nan("")));
            nullv("thr_a");
            nullv("thr_b");
            tel("thr_a_norm", 0.0);
            tel("thr_b_norm", 0.0);
            nullv("thr_a_v");
            nullv("thr_b_v");
            num("pwm_moe", started ? 1 : 0);
            num("gate_ready", gateReady ? 1 : 0);   // F36: flag-conditional
            num("gate_fault", gate_fault_latched ? 1 : 0);
            num("control_outputs_enabled", started ? 1 : 0);
            num("foc_running", driving ? 1 : 0);
            num("tim_isr_running", driving ? 1 : 0);
            num("enc_rejects", 0);
            num("cg_vlimit_scale", 1.0);   // stub: graph does not log it
            nullv("cg_vd_req_v");
            nullv("cg_vq_req_v");
            tel("cg_vu_v", getm(Meas_Vu_V, 0.0));
            tel("cg_vv_v", getm(Meas_Vv_V, 0.0));
            tel("cg_vw_v", getm(Meas_Vw_V, 0.0));
            nullv("cg_sample_age_us");
            nullv("cg_sample_seq");
            nullv("cg_sample_valid");
            num("tw_p_heat_w", getm(Meas_P_Heat_W, 0.0));
            num("tw_p_dc_w", getm(Meas_P_Dc_W, 0.0));
            num("tw_p_shaft_em_w", getm(Meas_P_ShaftEm_W, 0.0));
            sess.telemetry(t, v);
        }

        // ---- 1 Hz state records ----
        if (k % 2500 == 0) {
            sess.state(t, "control_state", started ? "\"RUNNING\"" : "\"IDLE\"");
            sess.state(t, "fault_flags_hex", "\"0x00000000\"");
            sess.state(t, "fault_active_names", "[]");
        }
    }

    sess.finish();
    std::fprintf(stderr, "wrote %s (%ld records)\n", args.outPath.c_str(), sess.n_records);
    return 0;
}

int main(int argc, char** argv) {
    // Exit-4 uniformity: an uncreatable --out must surface as a diagnostic
    // (the fopen-based sess.open path below already returns 4), never as
    // an uncaught-exception abort (exit 134) — any std::exception escaping
    // plant boot, the writer, or the loop lands here with its what() text.
    try {
        return RunScenario(argc, argv);
    } catch (const std::filesystem::filesystem_error& e) {
        std::fprintf(stderr, "sim_host: filesystem error: %s\n", e.what());
        return 4;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sim_host: fatal: %s\n", e.what());
        return 4;
    }
}
