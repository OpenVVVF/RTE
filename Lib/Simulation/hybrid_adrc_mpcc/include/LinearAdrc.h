#pragma once

/**
 * First-order Linear ADRC / ESO (Gao bandwidth parameterization).
 * Matches RTE Assets/NodeTemplates/Control.Ladrc — used here only as an
 * observer that estimates total disturbance for hybrid MPC.
 *
 * Plant form:  dy/dt = f + b0 * u
 * ESO:
 *   e  = z1 - y
 *   z1 += dt * (z2 + b0*u_prev - 2*ωo*e)
 *   z2 += dt * (-ωo^2 * e)
 */

#include <cmath>

namespace hybrid_adrc_mpcc {

struct LinearAdrcParams {
    float dt = 200e-6f;
    float b0 = 10000.0f;     // ≈ 1/L for current loop
    float omega_c = 800.0f;  // controller bandwidth (unused in ESO-only mode)
    float omega_o = 2400.0f; // observer bandwidth
    float u_max = 1.0e6f;
    float u_min = -1.0e6f;
};

class LinearAdrc {
public:
    explicit LinearAdrc(LinearAdrcParams p = {}) : p_(p) {}

    void reset(float y0 = 0.0f) {
        z1_ = y0;
        z2_ = 0.0f;
        u_prev_ = 0.0f;
        cold_ = true;
    }

    const LinearAdrcParams& params() const { return p_; }
    LinearAdrcParams& params() { return p_; }

    float z1() const { return z1_; }
    float z2() const { return z2_; }  // estimated total disturbance f̂
    float u_prev() const { return u_prev_; }

    /** Update ESO with measurement y and previously applied input u_prev. */
    void updateObserver(float y, float u_applied) {
        float dt = p_.dt;
        float b0 = p_.b0;
        float wo = p_.omega_o;
        if (!(dt > 0.0f)) dt = 200e-6f;
        if (!(std::fabs(b0) > 1.0e-6f)) b0 = 10000.0f;
        if (!(wo > 0.0f)) wo = 2400.0f;

        if (cold_) {
            z1_ = y;
            z2_ = 0.0f;
            cold_ = false;
        }

        const float beta1 = 2.0f * wo;
        const float beta2 = wo * wo;
        const float e = z1_ - y;
        z1_ += dt * (z2_ + b0 * u_applied - beta1 * e);
        z2_ += dt * (-beta2 * e);
        u_prev_ = u_applied;
    }

    /**
     * Classic LADRC control law (for standalone ADRC tests if needed):
     *   u0 = ωc * (r - z1);  u = (u0 - z2) / b0
     */
    float control(float setpoint, float y, float u_applied_prev) {
        updateObserver(y, u_applied_prev);
        float b0 = p_.b0;
        float wc = p_.omega_c;
        if (!(std::fabs(b0) > 1.0e-6f)) b0 = 10000.0f;
        if (!(wc > 0.0f)) wc = 800.0f;
        float u = (wc * (setpoint - z1_) - z2_) / b0;
        if (u > p_.u_max) u = p_.u_max;
        if (u < p_.u_min) u = p_.u_min;
        u_prev_ = u;
        return u;
    }

private:
    LinearAdrcParams p_;
    float z1_ = 0.0f;
    float z2_ = 0.0f;
    float u_prev_ = 0.0f;
    bool cold_ = true;
};

}  // namespace hybrid_adrc_mpcc
