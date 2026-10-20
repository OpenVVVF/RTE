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
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
 * SessionWriter (same field order, same %.9g formatting, same n_records
 * accounting: console/command/telemetry bump it, state does not).
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

    bool open(const char* path, const std::string& scenario) {
        f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f, "%s\n", RTESim::FormatSessionStartLine(scenario, "rte-sim").c_str());
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
        "  --dt-sub S               plant substep (default 0.0002)\n"
        "  --dyno-rpm R             hold dyno at R mechanical rpm\n"
        "  --dyno-series 't=rpm,t=rpm,...'   piecewise-constant dyno schedule\n"
        "  --supply 'v_set=..,i_limit=..,sink_limit=..,can_sink=0|1'\n"
        "  --event 't=.. kind=.. arg=..'     repeatable; kinds: reset, start,\n"
        "                           var-set (arg=Name=Value), fault-inject (arg=Name),\n"
        "                           fault-clear (arg=Name|empty=all), dyno-set (arg=rpm)\n");
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
        std::string v;
        if (a == "--out") { if (!next("--out", args.outPath)) return false; }
        else if (a == "--name") { if (!next("--name", args.name)) return false; }
        else if (a == "--duration") { if (!next("--duration", v)) return false; args.duration = std::atof(v.c_str()); }
        else if (a == "--telemetry-hz") { if (!next("--telemetry-hz", v)) return false; args.telemetryHz = std::atof(v.c_str()); }
        else if (a == "--dt-sub") { if (!next("--dt-sub", v)) return false; args.dtSub = std::atof(v.c_str()); }
        else if (a == "--dyno-rpm") { if (!next("--dyno-rpm", v)) return false; args.dynoRpm = std::atof(v.c_str()); }
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
        else if (a == "--help" || a == "-h") { Usage(); std::exit(2); }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return false; }
    }
    if (args.outPath.empty()) {
        std::fprintf(stderr, "--out is required\n");
        return false;
    }
    if (args.duration <= 0.0 || args.dtSub <= 0.0 || args.dtSub > kDt ||
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

int main(int argc, char** argv) {
    HostArgs args;
    if (!ParseArgs(argc, argv, args)) { Usage(); return 2; }

    // plant boot (generated plant or staged fallback; identical API)
    plant::PlantState plantState;
    plant::PlantInit(plantState);
    if (args.supply.any()) {
        // per-field fallback fill = the gen7 bench defaults (the same values
        // the fallback plant and the gen7_bench_pmsm DcBus node carry)
        plant::PlantSetSupply(plantState,
            args.supply.hasVSet ? args.supply.vSet : 144.7,
            args.supply.hasILimit ? args.supply.iLimit : 400.0,
            args.supply.hasSinkLimit ? args.supply.sinkLimit : 50.0,
            args.supply.hasCanSink ? args.supply.canSink : true);
    }
    plant::PlantSetDynoRpm(plantState, static_cast<float>(args.dynoRpm));

    plant::PlantMeas meas{};
    plant::PlantCollectMeas(plantState, meas);

    rtesim_host::ShimRuntime& shim = rtesim_host::ShimRuntime::instance();
    shim.plant = &plantState;
    shim.meas = &meas;
    // FRAM seed: same controller parameters the python replica believes in
    // (FocController.DEFAULTS), so both engines run identical control law inputs.
    // (verbatim from twin/native/src/main.cpp)
    shim.config = {
        {"Motor.Encoder.SinCos.Sign", 1.0f},
        {"Motor.Encoder.SinCos.OffsetDeg", 33.034f},
        {"Motor.Poles", 10.0f},
        {"Ctrl.PiQ.Kp", 0.03f},
        {"Ctrl.PiQ.Ki", 10.0f},
        {"Ctrl.PiD.Kp", 0.03f},
        {"Ctrl.PiD.Ki", 10.0f},
        {"Ctrl.Iq.SlewAps", 50.0f},
        {"Ctrl.ThrottleEnable", 1.0f},
        {"Motor.Ld", 100e-6f},
        {"Motor.Lq", 100e-6f},
        {"Motor.Lambda", 0.040f},
    };

    SessionWriter sess;
    if (!sess.open(args.outPath.c_str(), args.name)) {
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
        // plant stator resistance on the Gen7 bench (gen7_bench_pmsm Machine
        // Rs=0.00765 -> %.4f prints 0.0076 like the monolith recordings)
        constexpr double kPlantRsForBootLog = 0.00765;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "[CFG] motor config loaded from FRAM: type=pmsm_ipm poles=10 "
                      "offset=33.034 deg sign=1 R=%.4f ohm kp=0.0300 ki=10.0000",
                      kPlantRsForBootLog);
        sess.console(0.0, buf);
    }
    sess.console(0.0, "[SHELL] Command shell ready; type HELP for list");
    sess.console(0.0, "[CUR] start cal done U=-4.565 V=-2.482");

    bool gate_fault_latched = true;   // boot latch like hardware
    bool running = false;

    std::size_t ev_i = 0;
    std::size_t dyno_i = 0;

    const int nSub = std::max(1, static_cast<int>(std::lround(kDt / args.dtSub)));
    const long publishEvery =
        std::max(1L, std::lround(1.0 / (args.telemetryHz * kDt)));

    const long n_steps = static_cast<long>(args.duration / kDt);
    for (long k = 0; k < n_steps; ++k) {
        const double t = k * kDt;
        shim.now_us = static_cast<unsigned long long>(t * 1e6 + 0.5);

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
                    running = true;
                    sess.console(t, "[SUP] STARTED f_sw=2500 Hz f_u=5000 Hz");
                    sess.console(t, "[SHELL] control started: MAIN motor control (RTE graph)");
                }
            } else if (ev.kind == "var-set") {
                const std::size_t eq = ev.arg.find('=');
                const std::string varName = ev.arg.substr(0, eq);
                const float value = static_cast<float>(std::atof(ev.arg.c_str() + eq + 1));
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
                if (!found) {
                    std::fprintf(stderr, "warning: unknown runtime var '%s'\n", varName.c_str());
                }
                sess.console(t, echo);
            } else if (ev.kind == "fault-inject") {
                char cmd[128];
                std::snprintf(cmd, sizeof cmd, "fault inject %s", ev.arg.c_str());
                sess.command(t, cmd);
                plant::PlantSetFault(plantState, ev.arg.c_str(), true);
                char line[160];
                std::snprintf(line, sizeof line, "[PLANT] fault injected: %s", ev.arg.c_str());
                sess.console(t, line);
            } else if (ev.kind == "fault-clear") {
                const std::string which = ev.arg.empty() ? "all" : ev.arg;
                char cmd[128];
                std::snprintf(cmd, sizeof cmd, "fault clear %s", which.c_str());
                sess.command(t, cmd);
                if (ev.arg.empty()) PlantSetAllFaults(plantState, false);
                else plant::PlantSetFault(plantState, ev.arg.c_str(), false);
                char line[160];
                std::snprintf(line, sizeof line, "[PLANT] fault cleared: %s", which.c_str());
                sess.console(t, line);
            } else if (ev.kind == "dyno-set") {
                const double rpm = std::atof(ev.arg.c_str());
                char cmd[96], line[128];
                std::snprintf(cmd, sizeof cmd, "dyno set %g", rpm);
                std::snprintf(line, sizeof line, "[PLANT] dyno speed set to %g rpm", rpm);
                sess.command(t, cmd);
                plant::PlantSetDynoRpm(plantState, static_cast<float>(rpm));
                sess.console(t, line);
            }
        }

        // ---- dyno speed series (piecewise constant) ----
        while (dyno_i < args.dynoSeries.size() && t >= args.dynoSeries[dyno_i].t - 1e-12) {
            plant::PlantSetDynoRpm(plantState, static_cast<float>(args.dynoSeries[dyno_i].rpm));
            ++dyno_i;
        }

        // ---- 2.5 kHz domains: adc burst sampling always live; control gated ----
        platform_set_current_domain_dt(static_cast<float>(kDt));
        app::AdcIsrStep(adc);
        if (running) {
            platform_set_current_domain_dt(static_cast<float>(kDt));
            app::TimIsrStep(tim);
        }

        // ---- plant: freeze the duty vector for the control period, substep ----
        if (running) {
            plant::PlantSetDuty(plantState, shim.pwm_du, shim.pwm_dv, shim.pwm_dw);
        } else {
            plant::PlantSetDuty(plantState, 50.0f, 50.0f, 50.0f);
        }
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

            num("cg_du", shim.pwm_du);
            num("cg_dv", shim.pwm_dv);
            num("cg_dw", shim.pwm_dw);
            if (running) {
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
            tel("Mech_RPM", 0.0);
            tel("Elec_RPM", 0.0);
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
            num("pwm_moe", running ? 1 : 0);
            num("gate_ready", 1);
            num("gate_fault", gate_fault_latched ? 1 : 0);
            num("control_outputs_enabled", running ? 1 : 0);
            num("foc_running", running ? 1 : 0);
            num("tim_isr_running", running ? 1 : 0);
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
            sess.state(t, "control_state", running ? "\"RUNNING\"" : "\"IDLE\"");
            sess.state(t, "fault_flags_hex", "\"0x00000000\"");
            sess.state(t, "fault_active_names", "[]");
        }
    }

    sess.finish();
    std::fprintf(stderr, "wrote %s (%ld records)\n", args.outPath.c_str(), sess.n_records);
    return 0;
}
