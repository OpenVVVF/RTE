#pragma once

#include "motor_model.h"
#include "plant/plant_backend.h"
#include "sim_context.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace hostsim {

enum class StimulusType { Constant, Ramp, Step };

struct StimulusProfile {
    StimulusType type = StimulusType::Constant;
    float value = 0.0f;
    float start = 0.0f;
    float end = 0.0f;
    float start_s = 0.0f;
    float end_s = 0.0f;
    float step_time_s = 0.0f;
    float step_value = 0.0f;
};

/* Scenario-driven CAN frame injector. period_s > 0 repeats from start_s,
 * otherwise a single frame at start_s. */
struct SimCanInjectFrame {
    uint8_t bus = 1;
    uint32_t id = 0;
    bool ext = false;
    uint8_t dlc = 0;
    uint8_t data[8] = {0};
    float start_s = 0.0f;
    float period_s = 0.0f;
    float next_fire_s = 0.0f;
    bool done = false;
};

struct SimConfig {
    float duration_s = 1.0f;
    float tim_isr_hz = 10000.0f;
    float adc_isr_hz = 10000.0f;
    float app_loop_hz = 1000.0f;
    float telem_hz = 500.0f;
    float pwm_telem_hz = 0.0f; /* 0 = auto from carrier when pwm scope live */
    float realtime_factor = 1.0f;
    bool pwm_scope_enabled = false;
    float pwm_carrier_hz = 800.0f;
    bool live = false;
    /* Legacy scheduler-synthesized SPWM when no graph node drives the duties.
     * Hidden fallback — must be explicitly requested with "demo_fallback". */
    bool demo_fallback = false;
    std::string listen_host = "127.0.0.1";
    int listen_port = 14608;
    std::string trace_csv = "trace.csv";
    std::string config_file; /* "" = in-memory config store only */
    std::string plant_backend = "ode";
    /* ngspice backend interpretation: "motor" (3-phase inverter semantics,
     * back-EMF, mechanics) or "dcdc" (per-leg duty*VDC into a converter
     * netlist; leg currents/bus voltages instead of motor state). Only
     * meaningful with backend "ngspice". */
    std::string plant_mode = "motor";
    /* dcdc-mode default duties [%], applied every control step unless the
     * graph actually wrote PWM in that tick (ctx.pwm_written). Live duty
     * overrides still take precedence over both. */
    float dcdc_duty_u_pct = 0.0f;
    float dcdc_duty_v_pct = 0.0f;
    float dcdc_duty_w_pct = 0.0f;
    std::string ngspice_netlist = "";
    int ngspice_substeps = 4;
    MotorParams motor{};
    /* Graph Var node seeds ("vars": {"NodeId": value}) applied to the emitted
     * var registries after domain init — batch-mode equivalent of the live
     * firmware's `var set` (e.g. TargetHz in induction_vhz). */
    std::vector<std::pair<std::string, float>> graph_vars{};
    StimulusProfile throttle_a{};
    StimulusProfile throttle_b{};
    SimAdcConfig adc{};
    bool can_loopback = true;
    std::vector<SimCanInjectFrame> can_frames{};
    float motor_temp_c = 25.0f;
    float inverter_temp_c = 25.0f;
    /* Fault injection: 0/absent = disabled. */
    float overcurrent_a = 0.0f;          /* trip when any |i_phase| exceeds */
    float undervoltage_v = 0.0f;         /* trip when DC link below         */
    float vdc_glitch_time_s = -1.0f;     /* at t, drop DC link to ...       */
    float vdc_glitch_v = 0.0f;
};

class SimRuntime {
public:
    SimRuntime();
    ~SimRuntime();

    bool LoadScenario(const char* path);
    void InitDomains();
    bool StepOnce();
    void Shutdown();

    /* Batch: run until duration_s. Live: run until quit / Ctrl-C with TCP telemetry. */
    int Run();

    void SetLive(bool live) { config_.live = live; }
    void SetListen(const std::string& host, int port) {
        config_.listen_host = host;
        config_.listen_port = port;
    }
    void SetRealtimeFactor(float factor);
    void SetTelemetryHz(float hz) { config_.telem_hz = hz; }
    void ResetWallClockAnchor();

    const SimConfig& Config() const { return config_; }
    IPlant& Plant() { return *plant_; }
    const IPlant& Plant() const { return *plant_; }

    float TimeSeconds() const { return time_s_; }
    uint64_t TimeMicros() const {
        return static_cast<uint64_t>(time_s_ * 1.0e6);
    }

    float EvaluateStimulus(const StimulusProfile& profile) const;

    /* PWM scope publish rate scaled by 1/speed so slow motion keeps sample density. */
    float EffectivePwmTelemHz() const;

    void PublishPwmScopeFrame();

private:
    SimConfig config_{};
    std::unique_ptr<IPlant> plant_{};
    std::ofstream trace_{};

    float time_s_ = 0.0f;
    float next_tim_s_ = 0.0f;
    float next_adc_s_ = 0.0f;
    float next_app_s_ = 0.0f;
    float tim_dt_s_ = 1.0e-4f;
    float adc_dt_s_ = 1.0e-4f;
    float app_dt_s_ = 1.0e-3f;

    float throttle_a_ = 0.0f;
    float throttle_b_ = 0.0f;
    float duty_u_ = 0.0f;
    float duty_v_ = 0.0f;
    float duty_w_ = 0.0f;
    float next_telem_s_ = 0.0f;
    float next_pwm_telem_s_ = 0.0f;

    bool demo_fallback_warned_ = false;
    bool vdc_glitch_applied_ = false;
    bool overcurrent_raised_ = false;
    bool undervoltage_raised_ = false;
    /* Resolved dcdc mode: scenario asked for it, backend is ngspice, and the
     * plant still is an NgspicePlant whose netlist matched (no fallback). */
    bool dcdc_mode_ = false;
    /* Trace schema extension: dcdc runs append v_bus1..3,i_leg1..3 columns. */
    bool trace_dcdc_ = false;

    bool ParseScenario(const char* path);
    void ApplyGraphVars();
    void OpenTrace();
    void WriteTraceRow();
    void PublishTelemetry();
    void PublishPwmScopeTelemetry();
    void PaceRealtimeWallClock() const;

    std::chrono::steady_clock::time_point wall_anchor_{};
    float sim_anchor_s_ = 0.0f;
};

SimRuntime& GlobalSimRuntime();

} // namespace hostsim
