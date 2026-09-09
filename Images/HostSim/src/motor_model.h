#pragma once

#include <cmath>

namespace hostsim {

struct MotorParams {
    float rs_ohm = 0.05f;
    float ld_h = 0.0001f;
    float lq_h = 0.0001f;
    float flux_wb = 0.01f;
    int pole_pairs = 7;
    float inertia_kg_m2 = 1.0e-5f;
    float friction_nm_per_rad_s = 1.0e-4f;
    float vdc_v = 48.0f;
};

struct MotorState {
    float id_a = 0.0f;
    float iq_a = 0.0f;
    float theta_e_rad = 0.0f;
    float omega_e_rad_s = 0.0f;
    float ia_a = 0.0f;
    float ib_a = 0.0f;
    float ic_a = 0.0f;
};

class MotorModel {
public:
    void SetParams(const MotorParams& params);
    const MotorParams& Params() const { return params_; }
    const MotorState& State() const { return state_; }

    void Reset();
    void Step(float duty_u_pct, float duty_v_pct, float duty_w_pct, float dt_s);

    float ThetaElectricalDeg() const;
    float OmegaElectricalRadPerSec() const { return state_.omega_e_rad_s; }

private:
    MotorParams params_{};
    MotorState state_{};

    static float ClampDuty(float duty_pct);
    static void DutiesToAbcVoltage(float du, float dv, float dw, float vdc,
                                   float* va, float* vb, float* vc);
    static void AbcToDq(float va, float vb, float vc, float theta,
                        float* vd, float* vq);
    static void DqToAbc(float id, float iq, float theta,
                        float* ia, float* ib, float* ic);
};

} // namespace hostsim
