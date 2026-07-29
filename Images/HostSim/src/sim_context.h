#pragma once

namespace hostsim {

struct SimContext {
    float duty_u = 0.0f;
    float duty_v = 0.0f;
    float duty_w = 0.0f;
    float throttle_a = 0.0f;
    float throttle_b = 0.0f;
    float vdc_v = 48.0f;
    bool critical_fault = false;
    bool encoder_sample_new = false;
    uint64_t time_us = 0;
};

SimContext& GetSimContext();
void SimNotifyEncoderSample();

void SimRuntime_RegisterMotor(MotorModel* motor);

} // namespace hostsim
