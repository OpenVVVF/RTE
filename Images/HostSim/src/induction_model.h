#pragma once

/* ============================================================================
 * Squirrel-cage induction machine ODE model (stationary alpha/beta frame).
 *
 * Header-only on purpose: HostSIL compiles HostSim's src/motor_model.cpp
 * directly by relative path, and both images' CMake lists name sources
 * explicitly — an extra .cpp would not be picked up. MotorModel includes
 * this header and forwards to the model when MotorParams::machine is
 * Induction.
 *
 * Frame choice: stationary (stator-fixed) alpha/beta, because the drive
 * input is already a stationary-frame quantity (duties -> phase terminal
 * volts -> Clarke) and the V/Hz-style graphs emit V_Alpha/V_Beta. The
 * rotor-flux angle (and hence slip) falls out of the flux states via
 * atan2/lambda magnitude — no frame-tracking divisions that misbehave at
 * the 0 Hz start of a V/Hz ramp.
 *
 * States: stator current (i_sa, i_sb) and rotor flux linkage (l_ra, l_rb),
 * all referred to the stator.  With Ls = Lm + Lls, Lr = Lm + Llr,
 * sigma = 1 - Lm^2/(Ls*Lr), k = Lm/Lr:
 *
 *   d(l_ra)/dt = -(Rr/Lr) l_ra + (Rr*Lm/Lr) i_sa - w_e l_rb
 *   d(l_rb)/dt = -(Rr/Lr) l_rb + (Rr*Lm/Lr) i_sb + w_e l_ra
 *   sigma*Ls di_s/dt = v_s - Rs i_s - k d(l_r)/dt
 *   Te = (3/2) pp k (l_ra i_sb - l_rb i_sa)
 *
 * Mechanics deliberately mirror MotorModel: the electrical speed is
 * integrated directly as  d(w_e)/dt = (Te - B w_e)/J, and theta_e wraps in
 * [0, 2pi).  Same convention as the PMSM path, so scenario inertia/friction
 * mean the same thing for both machine types and the HostSIL rpm conversions
 * stay valid.
 * ========================================================================== */

#include <algorithm>
#include <cmath>

namespace hostsim {

struct InductionParams {
    float rs_ohm = 0.4f;
    float rr_ohm = 0.3f;
    float lm_h = 0.025f;
    float lls_h = 0.002f;
    float llr_h = 0.002f;
    int pole_pairs = 2;
    float inertia_kg_m2 = 1.0e-3f;
    float friction_nm_per_rad_s = 1.0e-4f;
};

class InductionMachine {
public:
    void SetParams(const InductionParams& params) { params_ = params; }
    const InductionParams& Params() const { return params_; }

    void Reset() {
        is_alpha_ = is_beta_ = 0.0f;
        lambda_r_alpha_ = lambda_r_beta_ = 0.0f;
        theta_e_rad_ = 0.0f;
        omega_e_rad_s_ = 0.0f;
    }

    /* One timestep with the stationary-frame stator voltage vector. */
    void Step(float vs_alpha, float vs_beta, float dt_s) {
        if (dt_s <= 0.0f) return;

        const float ls = params_.lm_h + params_.lls_h;
        const float lr = params_.lm_h + params_.llr_h;
        const float kr = (lr > 1e-12f) ? params_.lm_h / lr : 0.0f;
        const float rr_over_lr = (lr > 1e-12f) ? params_.rr_ohm / lr : 0.0f;
        const float sigma_ls =
            std::max(ls - params_.lm_h * params_.lm_h / std::max(lr, 1e-12f),
                     1e-9f);
        const float omega = omega_e_rad_s_;

        const float dlra = -rr_over_lr * lambda_r_alpha_ +
                           rr_over_lr * params_.lm_h * is_alpha_ -
                           omega * lambda_r_beta_;
        const float dlrb = -rr_over_lr * lambda_r_beta_ +
                           rr_over_lr * params_.lm_h * is_beta_ +
                           omega * lambda_r_alpha_;
        const float disa = (vs_alpha - params_.rs_ohm * is_alpha_ -
                            kr * dlra) / sigma_ls;
        const float disb = (vs_beta - params_.rs_ohm * is_beta_ -
                            kr * dlrb) / sigma_ls;

        is_alpha_ += disa * dt_s;
        is_beta_ += disb * dt_s;
        lambda_r_alpha_ += dlra * dt_s;
        lambda_r_beta_ += dlrb * dt_s;

        const float torque = 1.5f * static_cast<float>(params_.pole_pairs) *
                             kr * (lambda_r_alpha_ * is_beta_ -
                                   lambda_r_beta_ * is_alpha_);
        const float friction = params_.friction_nm_per_rad_s * omega;
        omega_e_rad_s_ += (torque - friction) / params_.inertia_kg_m2 * dt_s;
        theta_e_rad_ += omega * dt_s;
        constexpr float kTwoPi = 6.28318530717958647692f;
        while (theta_e_rad_ >= kTwoPi) theta_e_rad_ -= kTwoPi;
        while (theta_e_rad_ < 0.0f) theta_e_rad_ += kTwoPi;
    }

    float IsAlpha() const { return is_alpha_; }
    float IsBeta() const { return is_beta_; }
    float ThetaERad() const { return theta_e_rad_; }
    float OmegaERadS() const { return omega_e_rad_s_; }

    /* Rotor-flux-frame decomposition of the stator current: d = magnetizing
     * (along the rotor flux), q = torque-producing. |lambda| below epsilon
     * means the machine is unexcited and both read zero. */
    void FluxFrameCurrents(float* id_a, float* iq_a) const {
        const float mag = std::sqrt(lambda_r_alpha_ * lambda_r_alpha_ +
                                    lambda_r_beta_ * lambda_r_beta_);
        if (mag < 1e-9f) {
            if (id_a) *id_a = 0.0f;
            if (iq_a) *iq_a = 0.0f;
            return;
        }
        const float ca = lambda_r_alpha_ / mag;
        const float cb = lambda_r_beta_ / mag;
        if (id_a) *id_a = is_alpha_ * ca + is_beta_ * cb;
        if (iq_a) *iq_a = -is_alpha_ * cb + is_beta_ * ca;
    }

    /* Classical slip frequency w_slip = (Rr*Lm/Lr) * i_sq / |lambda_r|,
     * i.e. how much faster the flux vector rotates than the rotor shaft (at
     * no load near sync this decays toward 0). */
    float SlipElectricalRadPerSec() const {
        const float lr = params_.lm_h + params_.llr_h;
        const float mag2 = lambda_r_alpha_ * lambda_r_alpha_ +
                           lambda_r_beta_ * lambda_r_beta_;
        if (mag2 < 1e-18f || lr <= 1e-12f) return 0.0f;
        float id = 0.0f, iq = 0.0f;
        FluxFrameCurrents(&id, &iq);
        (void)id;
        const float mag = std::sqrt(mag2);
        return (params_.rr_ohm * params_.lm_h / lr) * iq / mag;
    }

private:
    InductionParams params_{};
    float is_alpha_ = 0.0f;
    float is_beta_ = 0.0f;
    float lambda_r_alpha_ = 0.0f;
    float lambda_r_beta_ = 0.0f;
    float theta_e_rad_ = 0.0f;
    float omega_e_rad_s_ = 0.0f;
};

} // namespace hostsim
