#pragma once

#include <cmath>

#include "induction_model.h"

namespace hostsim {

enum class MachineType { Pmsm = 0, Induction };

struct MotorParams {
    float rs_ohm = 0.05f;
    float ld_h = 0.0001f;
    float lq_h = 0.0001f;
    float flux_wb = 0.01f;
    int pole_pairs = 7;
    float inertia_kg_m2 = 1.0e-5f;
    float friction_nm_per_rad_s = 1.0e-4f;
    float vdc_v = 48.0f;
    /* Machine selection: Pmsm (default) integrates the salient dq PMSM below
     * and ignores the induction-only fields. */
    MachineType machine = MachineType::Pmsm;
    /* Induction machine (squirrel cage, stationary alpha/beta). rs_ohm,
     * pole_pairs, inertia, friction and vdc_v are shared with the PMSM. */
    float rr_ohm = 0.3f;
    float lm_h = 0.025f;
    float lls_h = 0.002f;
    float llr_h = 0.002f;
};

struct MotorState {
    float id_a = 0.0f;
    float iq_a = 0.0f;
    float theta_e_rad = 0.0f;
    float omega_e_rad_s = 0.0f;
    float ia_a = 0.0f;
    float ib_a = 0.0f;
    float ic_a = 0.0f;
    /* Phase terminal voltages applied by the inverter on the last Step()
     * (duty-derived, vs DC-). Read back by platform_phase_voltage_u/v/w so
     * telemetry sees what the plant actually received, not the duty request. */
    float va_v = 0.0f;
    float vb_v = 0.0f;
    float vc_v = 0.0f;
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
    /* Rotor-flux slip (induction only; 0 for PMSM). Diagnostic accessor for
     * scenario/debug sessions — the runtime trace derives slip from the
     * commanded feed frequency instead. */
    float SlipElectricalRadPerSec() const { return induction_.SlipElectricalRadPerSec(); }

private:
    MotorParams params_{};
    MotorState state_{};
    InductionMachine induction_{};

    static float ClampDuty(float duty_pct);
    static void DutiesToAbcVoltage(float du, float dv, float dw, float vdc,
                                   float* va, float* vb, float* vc);
    static void AbcToDq(float va, float vb, float vc, float theta,
                        float* vd, float* vq);
    static void DqToAbc(float id, float iq, float theta,
                        float* ia, float* ib, float* ic);
};

} // namespace hostsim
