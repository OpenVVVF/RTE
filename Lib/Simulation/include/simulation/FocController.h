#pragma once

#include <algorithm>
#include <cmath>

#include "simulation/PmsmPlant.h"
#include "simulation/Svpwm.h"
#include "simulation/Transforms.h"

namespace simulation {

struct FocParameters {
    float ts = 100e-6f;
    float kp_d = 5.0f;
    float ki_d = 3000.0f;
    float kp_q = 5.0f;
    float ki_q = 3000.0f;
    float aw_gain = 1.0f;
    PmsmParameters motor;
};

struct FocInputs {
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float vdc = 540.0f;
    bool enable = true;
};

struct FocOutputs {
    float valpha = 0.0f;
    float vbeta = 0.0f;
    float vd = 0.0f;
    float vq = 0.0f;
    bool valid = false;
};

/** Vector PI current control + inverse Park + SVPWM (RTE FOC path). */
class FocController {
public:
    explicit FocController(FocParameters params = {}) : params_(params) {}

    FocOutputs update(const FocInputs& in) {
        FocOutputs out;
        if (!in.enable || !(in.vdc > 0.0f) || !(params_.ts > 0.0f)) {
            return out;
        }

        const float v_limit = (in.vdc / kSqrt3) * 0.95f;

        const float err_d = in.id_ref - in.id;
        integral_d_ += err_d * params_.ts;
        float vd_raw = params_.kp_d * err_d + params_.ki_d * integral_d_;

        const float err_q = in.iq_ref - in.iq;
        integral_q_ += err_q * params_.ts;
        float vq_raw = params_.kp_q * err_q + params_.ki_q * integral_q_;

        // Decoupling feedforward (standard dq FOC).
        const float vd_ff = -in.omega_e * params_.motor.lq * in.iq;
        const float vq_ff = in.omega_e * params_.motor.ld * in.id + in.omega_e * params_.motor.psi_f;
        vd_raw += vd_ff;
        vq_raw += vq_ff;

        float vd = std::clamp(vd_raw, -v_limit, v_limit);
        float vq = std::clamp(vq_raw, -v_limit, v_limit);

        if (params_.aw_gain > 0.0f && params_.kp_d > 1e-4f && params_.ki_d > 1e-4f) {
            integral_d_ -= (vd_raw - vd) * params_.ts * params_.aw_gain / (params_.kp_d * params_.ki_d);
        }
        if (params_.aw_gain > 0.0f && params_.kp_q > 1e-4f && params_.ki_q > 1e-4f) {
            integral_q_ -= (vq_raw - vq) * params_.ts * params_.aw_gain / (params_.kp_q * params_.ki_q);
        }

        AlphaBeta v_ab;
        inverseParkDqToAlphaBeta({vd, vq}, in.theta_e, v_ab);
        const AlphaBeta v_pwm = svpwmAlphaBeta(v_ab.alpha, v_ab.beta, in.vdc);

        out.vd = vd;
        out.vq = vq;
        out.valpha = v_pwm.alpha;
        out.vbeta = v_pwm.beta;
        out.valid = true;
        return out;
    }

    void reset() {
        integral_d_ = 0.0f;
        integral_q_ = 0.0f;
    }

private:
    FocParameters params_;
    float integral_d_ = 0.0f;
    float integral_q_ = 0.0f;
};

}  // namespace simulation
