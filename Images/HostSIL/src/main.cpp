/*
 * host_sil — firmware-in-the-loop SIL simulator.
 *
 * Boots the real Gen6FW application (unmodified, compiled for the host)
 * against the HostSim ODE PMSM plant on a simulated clock.  Every TIM1
 * update event the scheduler fires, in hardware order:
 *
 *   1. plant step with the latched PWM duties (ODE averaged-duty model)
 *   2. phase-current injected-conversion-complete (= ADC ISR)
 *   3. encoder sample (TIM2 10 kHz, or TIM1-synced while control runs)
 *   4. HAL_TIM_PeriodElapsedCallback (= TIM1 UP ISR -> FOC step)
 *
 * The firmware application loop runs at scenario app_loop_hz through a
 * cooperative rendezvous (see sil_rt.h / README.md).
 *
 * Usage:
 *   host_sil <scenario.json> [--realtime N] [--live [--port P]]
 *     --realtime N   wall-clock pacing factor (0 or omitted = as fast as
 *                    possible; 1 = realtime)
 *     --live         serve the firmware's own USART3 telemetry byte stream
 *                    (COBS-framed InverterProtocol) verbatim on a TCP port for
 *                    RTEStudio (--tcp H:P --protocol ivp); implies realtime 1.0
 *                    unless --realtime overrides it
 *     --port P       live listen port (default 14608, same as HostSim)
 *
 * In --live mode bytes a client sends (e.g. RTEStudio's text console, or a
 * raw tcp client) are forwarded verbatim into the firmware's huart3 IT-RX
 * path, so the Gen6FW CommandShell receives them exactly like minicom-typed
 * bytes on hardware (see sil/sil_live_server.h and silUartRxPoll).
 *
 * Scenario JSON: see scenarios/sil_foc_demo.json and src/scenario.h.
 */
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "scenario.h"

#include "sil_rt.h"
#include "sil_world.h"
#include "sil_hooks.h"
#include "sil_fw_console.h"
#include "sil_live_server.h"

#include "Inverter/AppState.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/Calibration/CalKvStore.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"

#include "domain_tim_isr_generated.h"

extern "C" void sil_fram_attach(const char* path);
extern "C" void sil_fram_detach_save(void);
void sil_hal_init();   /* sil_hal.cpp: peripheral register defaults */

namespace {

/* --------------------------------------------------------------------------
 * Fast-tick event bookkeeping (all times in sim microseconds)
 * ------------------------------------------------------------------------ */
struct Sched {
    double next_inj_us  = 0.0;   /* TIM1 TRGO => injected conversion       */
    double next_upd_us  = 0.0;   /* TIM1 update event => UP ISR            */
    double next_enc_us  = 0.0;   /* encoder DMA sample (free-run trigger)  */
    double next_app_us  = 0.0;   /* app_loop iteration                     */
    double next_trace_us = 0.0;  /* trace CSV row                          */

    double inj_period_us = 400.0;
    double upd_period_us = 200.0;
    double enc_period_us = 100.0;   /* TIM2: 10 kHz */
    double app_period_us = 1000.0;

    bool   control_posted = false;
};

sil::Scenario g_scn;
bool          g_failed = false;
Sched         g_sched;

/* Sorted timeline of scenario shell commands (from the "commands" block). */
std::vector<std::pair<float, std::string>> g_commands;
size_t                                     g_cmd_next = 0;

/* --------------------------------------------------------------------------
 * Fault injection (scenario "faults" block)
 *
 * Each tick the scheduler recomputes which injection windows are active and
 * writes the shared fault state consumed by the sensor shims.  Edges are
 * logged as host-side "[SIL] inject ..." lines; the firmware-visible effect
 * (bits latched in FaultManager) is logged separately by
 * pollFaultTransitions() and by the firmware console mirror.
 * ------------------------------------------------------------------------ */

bool faultWindowActive(float t_s, float t0_s, float dur_s) {
    if (t0_s < 0.0f || t_s < t0_s) return false;
    return dur_s <= 0.0f || t_s < t0_s + dur_s;
}

struct FaultPrevState {
    bool vdc = false;
    bool oc = false;
    bool enc_freeze = false;
    bool enc_loss = false;
    bool temp = false;
};
FaultPrevState g_fault_prev;

void applyFaultWindows() {
    SilWorld& w = silWorld();
    const float t = static_cast<float>(sil_rt_now_us()) / 1.0e6f;

    /* DC-link glitch: the sensed bus voltage and the plant's drive voltage
     * dip together (the firmware reads bus V through the MAX22530 shim). */
    const float vdc =
        faultWindowActive(t, g_scn.vdc_glitch_time_s, g_scn.vdc_glitch_duration_s)
            ? g_scn.vdc_glitch_v
            : g_scn.vdc_v;
    w.vdc_v = vdc;
    if (std::fabs(w.plant.Model().Params().vdc_v - vdc) > 1.0e-3f) {
        hostsim::MotorParams mp = w.plant.Model().Params();
        mp.vdc_v = vdc;
        w.plant.SetParams(mp);
    }
    const bool vdc_act = (vdc != g_scn.vdc_v);
    if (vdc_act != g_fault_prev.vdc) {
        std::printf("[SIL] t=%.3f inject vdc_glitch %s (bus %.1f V)\n",
                    static_cast<double>(t), vdc_act ? "ON" : "off",
                    static_cast<double>(vdc));
        g_fault_prev.vdc = vdc_act;
    }

    /* Phase overcurrent at the ADC conversion level. */
    const bool oc = faultWindowActive(t, g_scn.oc_inject_time_s,
                                      g_scn.oc_inject_duration_s);
    w.oc_fault_active = oc;
    w.oc_fault_phase = g_scn.oc_inject_phase;
    w.oc_fault_a = g_scn.oc_inject_a;
    if (oc != g_fault_prev.oc) {
        static const char* phase_name[3] = {"U", "V", "W"};
        const int ph = (g_scn.oc_inject_phase >= 0 && g_scn.oc_inject_phase <= 2)
                           ? g_scn.oc_inject_phase : 0;
        std::printf("[SIL] t=%.3f inject oc_inject %s (phase %s, %+.1f A)\n",
                    static_cast<double>(t), oc ? "ON" : "off",
                    phase_name[ph], static_cast<double>(g_scn.oc_inject_a));
        g_fault_prev.oc = oc;
    }

    /* Encoder stream faults. */
    const bool enc_freeze =
        faultWindowActive(t, g_scn.encoder_freeze_time_s,
                          g_scn.encoder_freeze_duration_s);
    w.encoder_frozen = enc_freeze;
    if (enc_freeze != g_fault_prev.enc_freeze) {
        std::printf("[SIL] t=%.3f inject encoder_freeze %s (no new samples)\n",
                    static_cast<double>(t), enc_freeze ? "ON" : "off");
        g_fault_prev.enc_freeze = enc_freeze;
    }
    const bool enc_loss =
        faultWindowActive(t, g_scn.encoder_loss_time_s,
                          g_scn.encoder_loss_duration_s);
    w.encoder_sig_lost = enc_loss;
    if (enc_loss != g_fault_prev.enc_loss) {
        std::printf("[SIL] t=%.3f inject encoder_loss %s (sin/cos to bias mid)\n",
                    static_cast<double>(t), enc_loss ? "ON" : "off");
        g_fault_prev.enc_loss = enc_loss;
    }

    /* Temperature channel override. */
    const bool temp = faultWindowActive(t, g_scn.temp_spike_time_s,
                                        g_scn.temp_spike_duration_s);
    for (int ch = 0; ch < 4; ++ch) {
        w.temp_c[ch] = (temp && ch == g_scn.temp_spike_channel)
                           ? g_scn.temp_spike_c
                           : NAN;
    }
    if (temp != g_fault_prev.temp) {
        std::printf("[SIL] t=%.3f inject temp_spike %s (ch%d -> %.1f C)\n",
                    static_cast<double>(t), temp ? "ON" : "off",
                    g_scn.temp_spike_channel,
                    static_cast<double>(g_scn.temp_spike_c));
        g_fault_prev.temp = temp;
    }
}

/* --------------------------------------------------------------------------
 * Firmware-observed fault transition log
 *
 * Watches FaultManager::activeFlags() from the scheduler side and prints
 * every edge with the sim timestamp; trips are accumulated for the end-of-run
 * summary.  The firmware's own "triggered" lines (with the FaultReason
 * string) appear shortly after via the console mirror ([FW ...]).
 * ------------------------------------------------------------------------ */
struct FaultTrip {
    double                t_s;
    const char*           source;     /* static storage: FaultMeta::name */
    const char*           description;
    char                  severity;   /* 'W' / 'H' / 'C' */
};
std::vector<FaultTrip> g_fault_history;
uint32_t               g_fault_flags_seen = 0;

char severityChar(Inverter::FaultSeverity s) {
    switch (s) {
        case Inverter::FaultSeverity::Warning:  return 'W';
        case Inverter::FaultSeverity::High:     return 'H';
        case Inverter::FaultSeverity::Critical: return 'C';
    }
    return '?';
}

void pollFaultTransitions() {
    const uint32_t flags = Inverter::FaultManager::instance().activeFlags();
    if (flags == g_fault_flags_seen) return;
    const double t = static_cast<double>(sil_rt_now_us()) / 1e6;
    const uint32_t raised = flags & ~g_fault_flags_seen;
    const uint32_t cleared = g_fault_flags_seen & ~flags;
    for (size_t i = 0; i < Inverter::FaultManager::metaCount(); ++i) {
        const auto* m = &Inverter::FaultManager::metaTable()[i];
        const uint32_t bit = static_cast<uint32_t>(m->source);
        if ((raised & bit) != 0U) {
            const char sev = severityChar(m->severity);
            std::printf("[SIL] t=%.3f fault raised: source=%s severity=%c (%s)\n",
                        t, m->name, sev, m->description);
            g_fault_history.push_back({t, m->name, m->description, sev});
        }
        if ((cleared & bit) != 0U) {
            std::printf("[SIL] t=%.3f fault cleared: source=%s\n", t, m->name);
        }
    }
    g_fault_flags_seen = flags;
}

/* One fast tick of modeled hardware.  Must only run while the firmware
 * context is blocked (sil_rt guarantees this at every call site). */
void fastTick() {
    SilWorld& w = silWorld();

    /* 0. Scenario fault windows feed the world state first, so every sensor
     * shim below already sees the injected values. */
    applyFaultWindows();

    /* 1. Plant step with the latched duties (0 V phases while not driving). */
    float du = 0.0f, dv = 0.0f, dw_ = 0.0f;
    const bool driving = silTimOutputsDriving();
    if (driving) {
        PWM_GetCurrentDuties(&du, &dv, &dw_);
    }
    w.plant.Step(du, dv, dw_, 1.0e-6f);

    /* Physical side-channels for the sensor shims. */
    if (driving) {
        w.phase_pole_v[0] = du * w.vdc_v / 100.0f;
        w.phase_pole_v[1] = dv * w.vdc_v / 100.0f;
        w.phase_pole_v[2] = dw_ * w.vdc_v / 100.0f;
        const auto& st = w.plant.State();
        const float p = w.phase_pole_v[0] * st.ia_a +
                        w.phase_pole_v[1] * st.ib_a +
                        w.phase_pole_v[2] * st.ic_a;
        w.dc_link_current_a = (w.vdc_v > 1.0f) ? (p / w.vdc_v) : 0.0f;
    } else {
        w.phase_pole_v[0] = w.phase_pole_v[1] = w.phase_pole_v[2] = 0.0f;
        w.dc_link_current_a = 0.0f;
    }

    sil_rt_advance_time_us(1);
    const uint64_t now = sil_rt_now_us();

    /* 2. TIM1 TRGO -> injected phase-current conversion (ADC ISR). */
    if (silPhaseCurrentAdcRunning() && silTimBaseRunning()) {
        const float inj_hz = silTimSwitchingHz();
        if (inj_hz > 0.0f) {
            const double period = 1.0e6 / static_cast<double>(inj_hz);
            if (static_cast<double>(now) >= g_sched.next_inj_us) {
                silPhaseCurrentAdcTrigger();
                g_sched.next_inj_us += period;
                if (g_sched.next_inj_us < static_cast<double>(now)) {
                    g_sched.next_inj_us = static_cast<double>(now) + period;
                }
            }
            g_sched.inj_period_us = period;
        }
    }

    /* 3. Encoder: free-running TIM2 stream (or taken per update event when
     * synchronized — handled below).  encoder_frozen drops the sample stream
     * entirely, so the firmware's sample-age bookkeeping goes stale. */
    if (silEncoderRunning() && !w.encoder_frozen && !silEncoderSyncTrigger()) {
        if (static_cast<double>(now) >= g_sched.next_enc_us) {
            silEncoderSampleFromPlant();
            g_sched.next_enc_us += g_sched.enc_period_us;
            if (g_sched.next_enc_us < static_cast<double>(now)) {
                g_sched.next_enc_us = static_cast<double>(now) + g_sched.enc_period_us;
            }
        }
    }

    /* 4. TIM1 update event -> PWM-period ISR (FOC control step). */
    if (silTimBaseRunning() && silTimUpdateIrqEnabled()) {
        const float upd_hz = silTimUpdateHz();
        if (upd_hz > 0.0f) {
            const double period = 1.0e6 / static_cast<double>(upd_hz);
            if (static_cast<double>(now) >= g_sched.next_upd_us) {
                /* The encoder trigger is TIM1-synchronized while control
                 * runs: sample it just before the control step. */
                if (silEncoderRunning() && silEncoderSyncTrigger() &&
                    !w.encoder_frozen) {
                    silEncoderSampleFromPlant();
                }
                silTimFireUpdateIrq();
                g_sched.next_upd_us += period;
                if (g_sched.next_upd_us < static_cast<double>(now)) {
                    g_sched.next_upd_us = static_cast<double>(now) + period;
                }
            }
            g_sched.upd_period_us = period;
        }
    }

    /* 5. Firmware-visible fault bookkeeping (source/severity edges with sim
     * timestamps; the firmware console mirror prints the matching lines). */
    pollFaultTransitions();
}

/* Throttle profile -> pin voltages for the slow-sensor shim. */
void updateThrottleVoltages() {
    SilWorld& w = silWorld();
    const float t_s = static_cast<float>(sil_rt_now_us() / 1000000ULL) +
                      static_cast<float>(sil_rt_now_us() % 1000000ULL) / 1.0e6f;
    const float a = g_scn.throttle_a.at(t_s);
    const float b = g_scn.throttle_b_set ? g_scn.throttle_b.at(t_s)
                                         : g_scn.throttle_a.at(t_s);
    /* 0.5 .. 4.5 V pin range (header default KV bounds). */
    w.throttle_a_v = 0.5f + a * 4.0f;
    w.throttle_b_v = 0.5f + b * 4.0f;
}

void writeTraceRow(FILE* f) {
    SilWorld& w = silWorld();
    float du = 0.0f, dv = 0.0f, dw_ = 0.0f;
    PWM_GetCurrentDuties(&du, &dv, &dw_);
    const auto& st = w.plant.State();

    float theta_m = st.theta_e_rad /
                    static_cast<float>(w.plant.Model().Params().pole_pairs);
    const float rpm_mech = st.omega_e_rad_s * 60.0f /
        (6.28318530718f * static_cast<float>(w.plant.Model().Params().pole_pairs));

    const float iq_ref = appState.tim_isr.IqGate.Out;
    const float id_meas = appState.tim_isr.Park.I_D.in(au::amperes);
    const float iq_meas = appState.tim_isr.Park.I_Q.in(au::amperes);

    std::fprintf(f, "%llu,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.1f,%.4f,%.4f,%.4f,%.4f\n",
                 (unsigned long long)sil_rt_now_us(),
                 static_cast<double>(du), static_cast<double>(dv),
                 static_cast<double>(dw_),
                 static_cast<double>(st.ia_a), static_cast<double>(st.ib_a),
                 static_cast<double>(st.ic_a),
                 static_cast<double>(st.theta_e_rad),
                 static_cast<double>(st.omega_e_rad_s),
                 static_cast<double>(theta_m),
                 static_cast<double>(rpm_mech),
                 static_cast<double>(w.vdc_v),
                 static_cast<double>(iq_ref),
                 static_cast<double>(id_meas),
                 static_cast<double>(iq_meas));
}

#define SIL_TRACE_HEADER \
    "time_us,duty_u,duty_v,duty_w,i_a,i_b,i_c,theta_e_rad,omega_e_rad_s," \
    "theta_m_rad,rpm_mech,vdc_v,iq_ref_a,id_meas_a,iq_meas_a\n"

/* Deferred firmware-context actions ------------------------------------- */

void runShellLines(const std::vector<std::string>* lines) {
    for (const auto& line : *lines) {
        CommandManager::instance().processLine(line.c_str());
    }
}

void applyFirmwareConfig() {
    for (const auto& kv : g_scn.firmware_config) {
        char line[128];
        std::snprintf(line, sizeof(line), "config set %s %g",
                      kv.first.c_str(), static_cast<double>(kv.second));
        CommandManager::instance().processLine(line);
        std::snprintf(line, sizeof(line), "config save %s", kv.first.c_str());
        CommandManager::instance().processLine(line);
    }
    /* Refresh the runtime motor calibration from the seeded KV store so the
     * platform rpm/feedforward helpers see matching poles/sign. */
    Inverter::CalKvStore::loadMotorCalibration();
    /* Same for the temperature channel config (firmware `temp reload`
     * equivalent) so seeded Hw.Temp.* / Motor.Temp.* keys take effect. */
    Inverter::appSensors().reloadConfig();
}

void engageControl() {
    if (g_scn.pwm_switching_hz > 0.0f) {
        PWM_SetFrequency(static_cast<uint32_t>(g_scn.pwm_switching_hz + 0.5f));
    }
    CommandManager::instance().processLine("control start");
    if (g_scn.iq_a != 0.0f || g_scn.id_a != 0.0f) {
        char line[128];
        std::snprintf(line, sizeof(line), "var set IqVar %g",
                      static_cast<double>(g_scn.iq_a));
        CommandManager::instance().processLine(line);
        std::snprintf(line, sizeof(line), "var set IdVar %g",
                      static_cast<double>(g_scn.id_a));
        CommandManager::instance().processLine(line);
    }
}

/* Host-side active-fault listing (works even without the --live link). */
void printActiveFaults() {
    const uint32_t flags = Inverter::FaultManager::instance().activeFlags();
    if (flags == 0) {
        std::printf("[SIL] no active faults\n");
        return;
    }
    for (size_t i = 0; i < Inverter::FaultManager::metaCount(); ++i) {
        const auto* m = &Inverter::FaultManager::metaTable()[i];
        if ((flags & static_cast<uint32_t>(m->source)) != 0) {
            std::printf("[SIL] FAULT active: %s (%s)\n", m->name, m->description);
        }
    }
}

} // namespace

/* Ensure the live server socket is closed on every early return path. */
struct LiveServerGuard {
    ~LiveServerGuard() { sil_live_stop(); }   /* safe when never started */
};

int main(int argc, char** argv) {
    const char* scenario_path = "scenarios/sil_foc_demo.json";
    float realtime = 0.0f;
    bool  realtime_set = false;
    bool  live = false;
    uint16_t live_port = SIL_LIVE_DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--realtime") == 0) {
            if (++i >= argc) {
                std::fprintf(stderr, "missing value for --realtime\n");
                return 1;
            }
            realtime = static_cast<float>(std::atof(argv[i]));
            realtime_set = true;
        } else if (std::strcmp(arg, "--live") == 0) {
            live = true;
        } else if (std::strcmp(arg, "--port") == 0) {
            if (++i >= argc) {
                std::fprintf(stderr, "missing value for --port\n");
                return 1;
            }
            const int p = std::atoi(argv[i]);
            if (p < 1 || p > 65535) {
                std::fprintf(stderr, "invalid port: %s\n", argv[i]);
                return 1;
            }
            live_port = static_cast<uint16_t>(p);
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::fprintf(stderr,
                         "usage: %s [scenario.json] [--realtime N] "
                         "[--live [--port P]]\n",
                         argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            scenario_path = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg);
            return 1;
        }
    }

    /* --live is meant to be watched from RTEStudio; default to realtime
     * pacing (an explicit --realtime still wins). */
    if (live && !realtime_set) {
        realtime = 1.0f;
    }

    std::string err;
    if (!sil::LoadScenario(scenario_path, g_scn, err)) {
        std::fprintf(stderr, "[SIL] %s\n", err.c_str());
        return 1;
    }

    /* Scheduled shell commands, sorted by absolute sim time (stable for
     * equal times). */
    g_commands = g_scn.commands;
    std::stable_sort(g_commands.begin(), g_commands.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    g_cmd_next = 0;

    /* Plant + static world config. */
    {
        hostsim::MotorParams mp;
        mp.rs_ohm = g_scn.rs_ohm;
        mp.ld_h = g_scn.ld_h;
        mp.lq_h = g_scn.lq_h;
        mp.flux_wb = g_scn.flux_wb;
        mp.pole_pairs = g_scn.pole_pairs;
        mp.inertia_kg_m2 = g_scn.inertia_kg_m2;
        mp.friction_nm_per_rad_s = g_scn.friction_nm_per_rad_s;
        mp.vdc_v = g_scn.vdc_v;
        if (g_scn.machine == "induction") mp.machine = hostsim::MachineType::Induction;
        mp.rr_ohm = g_scn.rr_ohm;
        mp.lm_h = g_scn.lm_h;
        mp.lls_h = g_scn.lls_h;
        mp.llr_h = g_scn.llr_h;
        silWorld().plant.SetParams(mp);
        silWorld().plant.Reset();
        silWorld().vdc_v = g_scn.vdc_v;
    }

    g_sched.app_period_us = 1.0e6 / static_cast<double>(g_scn.app_loop_hz);

    sil_fram_attach(g_scn.fram_image.empty() ? nullptr : g_scn.fram_image.c_str());
    sil_hal_init();
    silFwConsoleReset();

    FILE* trace = std::fopen(g_scn.trace_csv.c_str(), "w");
    if (trace == nullptr) {
        std::fprintf(stderr, "[SIL] cannot open trace_csv %s\n",
                     g_scn.trace_csv.c_str());
        return 1;
    }
    std::fputs(SIL_TRACE_HEADER, trace);

    std::printf("[SIL] scenario=%s duration=%.2f s app_loop=%.0f Hz trace=%s\n",
                scenario_path, static_cast<double>(g_scn.duration_s),
                static_cast<double>(g_scn.app_loop_hz),
                g_scn.trace_csv.c_str());
    std::printf("[SIL] motor: %s rs=%.4f ohm ld=%.1f uH lq=%.1f uH flux=%.4f Wb "
                "pp=%d J=%.2e B=%.2e vdc=%.1f\n",
                g_scn.machine.c_str(),
                static_cast<double>(g_scn.rs_ohm),
                static_cast<double>(g_scn.ld_h) * 1e6,
                static_cast<double>(g_scn.lq_h) * 1e6,
                static_cast<double>(g_scn.flux_wb), g_scn.pole_pairs,
                static_cast<double>(g_scn.inertia_kg_m2),
                static_cast<double>(g_scn.friction_nm_per_rad_s),
                static_cast<double>(g_scn.vdc_v));

    /* Announce configured fault injections and scheduled shell commands. */
    {
        auto window = [](float t0, float dur) -> std::string {
            char buf[64];
            if (dur > 0.0f) {
                std::snprintf(buf, sizeof(buf), "@%.3f s for %.3f s",
                              static_cast<double>(t0), static_cast<double>(dur));
            } else {
                std::snprintf(buf, sizeof(buf), "@%.3f s (latching)",
                              static_cast<double>(t0));
            }
            return buf;
        };
        if (g_scn.vdc_glitch_time_s >= 0.0f)
            std::printf("[SIL] fault cfg: vdc_glitch to %.1f V %s\n",
                        static_cast<double>(g_scn.vdc_glitch_v),
                        window(g_scn.vdc_glitch_time_s, g_scn.vdc_glitch_duration_s).c_str());
        if (g_scn.oc_inject_time_s >= 0.0f)
            std::printf("[SIL] fault cfg: oc_inject %.1f A phase %d %s\n",
                        static_cast<double>(g_scn.oc_inject_a), g_scn.oc_inject_phase,
                        window(g_scn.oc_inject_time_s, g_scn.oc_inject_duration_s).c_str());
        if (g_scn.encoder_freeze_time_s >= 0.0f)
            std::printf("[SIL] fault cfg: encoder_freeze %s\n",
                        window(g_scn.encoder_freeze_time_s, g_scn.encoder_freeze_duration_s).c_str());
        if (g_scn.encoder_loss_time_s >= 0.0f)
            std::printf("[SIL] fault cfg: encoder_loss %s\n",
                        window(g_scn.encoder_loss_time_s, g_scn.encoder_loss_duration_s).c_str());
        if (g_scn.temp_spike_time_s >= 0.0f)
            std::printf("[SIL] fault cfg: temp_spike ch%d to %.1f C %s\n",
                        g_scn.temp_spike_channel,
                        static_cast<double>(g_scn.temp_spike_c),
                        window(g_scn.temp_spike_time_s, g_scn.temp_spike_duration_s).c_str());
        for (const auto& [t_s, line] : g_commands)
            std::printf("[SIL] command cfg: @%.3f s: %s\n",
                        static_cast<double>(t_s), line.c_str());
    }

    /* --- Boot the firmware on its own thread. --- */
    LiveServerGuard live_guard;
    if (live && !sil_live_start("127.0.0.1", live_port)) {
        std::fprintf(stderr, "[SIL] --live requested but the listen socket "
                             "failed (see above); running without live link\n");
    }
    sil_rt_start_firmware();
    g_failed = !sil_rt_idle_to_gate(fastTick);
    if (g_failed) {
        std::fprintf(stderr, "[SIL] firmware failed during boot: %s\n",
                     sil_rt_fw_error());
        std::fclose(trace);
        sil_rt_shutdown();
        return 2;
    }
    std::printf("[SIL] firmware boot complete at t=%.3f s (sim)\n",
                static_cast<double>(sil_rt_now_us()) / 1e6);

    /* Post-boot config seeds (graph config live values + FRAM persistence). */
    if (!g_scn.firmware_config.empty()) {
        sil_rt_post(&applyFirmwareConfig);
        if (!sil_rt_run_app_iteration(fastTick)) {
            std::fprintf(stderr, "[SIL] firmware died applying config: %s\n",
                         sil_rt_fw_error());
            std::fclose(trace);
            sil_rt_shutdown();
            return 2;
        }
    }

    /* Main scheduling loop. */
    const uint64_t end_us =
        static_cast<uint64_t>(g_scn.duration_s * 1.0e6);
    const auto wall0 = std::chrono::steady_clock::now();
    const double wall_rate = (realtime > 0.0f) ? static_cast<double>(realtime)
                                               : 0.0;

    while (sil_rt_now_us() < end_us) {
        const uint64_t now = sil_rt_now_us();

        /* Due scenario shell commands (posted as one firmware-context call so
         * a burst can't clobber the single pending slot; executes at the app
         * gate below).  When commands fire this tick, control_start waits one
         * app tick — the post slot is non-queueing. */
        bool posted_command = false;
        if (g_cmd_next < g_commands.size() &&
            static_cast<double>(g_commands[g_cmd_next].first) * 1.0e6 <=
                static_cast<double>(now) + 1.0) {
            auto due_lines = std::make_shared<std::vector<std::string>>();
            while (g_cmd_next < g_commands.size() &&
                   static_cast<double>(g_commands[g_cmd_next].first) * 1.0e6 <=
                       static_cast<double>(now) + 1.0) {
                std::printf("[SIL] t=%.3f shell> %s\n",
                            static_cast<double>(now) / 1e6,
                            g_commands[g_cmd_next].second.c_str());
                due_lines->push_back(g_commands[g_cmd_next].second);
                ++g_cmd_next;
            }
            auto keepalive = due_lines;
            const auto* lines_ptr = due_lines.get();
            sil_rt_post([keepalive, lines_ptr]() { runShellLines(lines_ptr); });
            posted_command = true;
        }

        /* Control engagement at the scenario time. */
        if (g_scn.control_start && !g_sched.control_posted &&
            !posted_command &&
            now >= static_cast<uint64_t>(g_scn.control_start_time_s * 1.0e6)) {
            sil_rt_post(&engageControl);
            g_sched.control_posted = true;
            /* It executes at the next app-gate entry below. */
        }

        /* App-loop boundary. */
        if (static_cast<double>(now) >= g_sched.next_app_us) {
            updateThrottleVoltages();
            silUartPumpTxCompletion();
            sil_live_poll();
            silUartRxPoll();   /* deliver live-link client bytes to the shell */
            if (!sil_rt_run_app_iteration(fastTick)) {
                std::fprintf(stderr, "[SIL] firmware died in main loop: %s\n",
                             sil_rt_fw_error());
                g_failed = true;
                break;
            }
            g_sched.next_app_us = static_cast<double>(sil_rt_now_us()) +
                                  g_sched.app_period_us;

            /* Host-side health check after control start. */
            if (g_sched.control_posted) {
                auto& sup = Inverter::ControlSupervisor::instance();
                static bool s_reported = false;
                if (!s_reported) {
                    std::printf("[SIL] control state at t=%.3f s: %s "
                                "(faults active: %s)\n",
                                static_cast<double>(sil_rt_now_us()) / 1e6,
                                sup.stateName(),
                                Inverter::FaultManager::instance().isActive()
                                    ? "YES" : "no");
                    if (!sup.isRunning()) {
                        std::fprintf(stderr,
                                     "[SIL] ERROR: control failed to start\n");
                        Inverter::FaultManager::instance().printSummary();
                        g_failed = true;
                        break;
                    }
                    s_reported = true;
                }
            }

            /* Trace row cadence. */
            if (static_cast<double>(sil_rt_now_us()) >= g_sched.next_trace_us) {
                writeTraceRow(trace);
                g_sched.next_trace_us = static_cast<double>(sil_rt_now_us()) +
                                        static_cast<double>(g_scn.trace_decim_us);
            }
            continue;
        }

        fastTick();

        /* Realtime pacing once per simulated millisecond. */
        if (wall_rate > 0.0 && (sil_rt_now_us() % 1000ULL) == 0ULL) {
            const double sim_s = static_cast<double>(sil_rt_now_us()) / 1e6;
            const double wall_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              wall0).count();
            const double ahead_s = wall_s * wall_rate - sim_s;
            if (ahead_s < 0.0) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(-ahead_s / wall_rate));
            }
        }
    }

    std::fclose(trace);

    /* Diagnostics + teardown. */
    if (g_failed) {
        sil_fram_detach_save();
        sil_rt_shutdown();
        return 2;
    }

    std::printf("[SIL] simulation complete at t=%.3f s (sim)\n",
                static_cast<double>(sil_rt_now_us()) / 1e6);
    {
        pollFaultTransitions();   /* drain any late raise from the last tick */
        auto& sup = Inverter::ControlSupervisor::instance();
        std::printf("[SIL] final control state: %s (faults: %s)\n",
                    sup.stateName(),
                    Inverter::FaultManager::instance().isActive() ? "YES" : "no");
        printActiveFaults();
        if (g_fault_history.empty()) {
            std::printf("[SIL] fault history: none raised this run\n");
        } else {
            std::printf("[SIL] fault history (%zu trips raised this run):\n",
                        g_fault_history.size());
            for (const FaultTrip& trip : g_fault_history) {
                std::printf("[SIL]   t=%.3f [%c] %s (%s)\n",
                            trip.t_s, trip.severity, trip.source,
                            trip.description);
            }
        }
    }

    sil_fram_detach_save();
    sil_rt_shutdown();
    return 0;
}
