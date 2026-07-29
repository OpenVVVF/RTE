#pragma once

#include "motor_model.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

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

struct SimConfig {
    float duration_s = 1.0f;
    float tim_isr_hz = 10000.0f;
    float adc_isr_hz = 10000.0f;
    float app_loop_hz = 1000.0f;
    float telem_hz = 500.0f;
    float pwm_telem_hz = 0.0f; /* 0 = auto from carrier when pwm scope live */
    float realtime_factor = 1.0f;
    bool pwm_scope_enabled = true;
    float pwm_carrier_hz = 800.0f;
    bool live = false;
    std::string listen_host = "127.0.0.1";
    int listen_port = 14608;
    std::string trace_csv = "trace.csv";
    MotorParams motor{};
    StimulusProfile throttle_a{};
    StimulusProfile throttle_b{};
};

class SimRuntime {
public:
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
    MotorModel& Motor() { return motor_; }
    const MotorModel& Motor() const { return motor_; }

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
    MotorModel motor_{};
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

    bool ParseScenario(const char* path);
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
