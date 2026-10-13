#pragma once

/**
 * Hybrid ADRC + modulated FCS-MPCC current controller (new code only).
 *
 * Architecture (ESO-assisted predictive duty control):
 *   1) d/q Linear ESOs estimate total disturbance f̂_d, f̂_q from
 *      previously applied v_d, v_q and measured i_d, i_q.
 *   2) Deadbeat voltage from ADRC plant form:
 *        i* = i + Ts (f̂ + b0 v)  →  v = ((i*-i)/Ts - f̂) / b0
 *   3) Nearest active inverter vector + duty vs zero (Zhang Method II style)
 *      so Gen6-like low-L machines can track without one-step current blow-up.
 *
 * Does not modify MpccController.h, Control.Ladrc, or paper_foc_vs_mpc.cpp.
 */

#include <algorithm>
#include <cmath>
#include <limits>

#include "LinearAdrc.h"
#include "simulation/PmsmPlant.h"
#include "simulation/TwoLevelInverter.h"
#include "simulation/Transforms.h"

namespace hybrid_adrc_mpcc {

struct HybridParams {
    simulation::PmsmParameters motor;
    float ts = 200e-6f;
    float i_base = 20.0f;
    float i_max = 40.0f;
    float omega_o = 2500.0f;  // ESO bandwidth [rad/s]
    float v_limit_factor = 0.95f;
};

struct HybridInputs {
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float vdc = 48.0f;
    bool enable = true;
};

struct HybridOutputs {
    bool sa = false;
    bool sb = false;
    bool sc = false;
    simulation::SwitchingState switching_state = simulation::SwitchingState::S000;
    float predicted_id = 0.0f;
    float predicted_iq = 0.0f;
    float min_cost = std::numeric_limits<float>::infinity();
    float vd = 0.0f;
    float vq = 0.0f;
    float valpha = 0.0f;
    float vbeta = 0.0f;
    float duty_active = 0.0f;
    float fhat_d = 0.0f;
    float fhat_q = 0.0f;
    bool valid = false;
};

class AdrcMpccHybrid {
public:
    explicit AdrcMpccHybrid(HybridParams params = {}) : params_(params) { configureEsos(); }

    const HybridParams& parameters() const { return params_; }

    void setParameters(const HybridParams& params) {
        params_ = params;
        configureEsos();
    }

    void reset() {
        configureEsos();
        eso_d_.reset();
        eso_q_.reset();
        prev_cmd_ = {};
        vd_prev_ = 0.0f;
        vq_prev_ = 0.0f;
        initialized_ = false;
    }

    HybridOutputs update(const HybridInputs& in) {
        HybridOutputs out;
        if (!in.enable || !validate(in)) {
            out.sa = prev_cmd_.sa;
            out.sb = prev_cmd_.sb;
            out.sc = prev_cmd_.sc;
            out.switching_state = simulation::commandToSwitchingState(prev_cmd_);
            out.fhat_d = eso_d_.z2();
            out.fhat_q = eso_q_.z2();
            return out;
        }

        if (initialized_) {
            eso_d_.updateObserver(in.id, vd_prev_);
            eso_q_.updateObserver(in.iq, vq_prev_);
        } else {
            eso_d_.reset(in.id);
            eso_q_.reset(in.iq);
            initialized_ = true;
        }

        const float f_d = eso_d_.z2();
        const float f_q = eso_q_.z2();
        const float b0_d = eso_d_.params().b0;
        const float b0_q = eso_q_.params().b0;
        const float ts = params_.ts;

        out.fhat_d = f_d;
        out.fhat_q = f_q;

        // Deadbeat voltage in ADRC coordinates: v = ((i*-i)/Ts - f̂) / b0
        float vd_ref = ((in.id_ref - in.id) / ts - f_d) / b0_d;
        float vq_ref = ((in.iq_ref - in.iq) / ts - f_q) / b0_q;

        const float v_max = (in.vdc / simulation::kSqrt3) * params_.v_limit_factor;
        const float v_mag = std::sqrt(vd_ref * vd_ref + vq_ref * vq_ref);
        if (v_mag > v_max && v_mag > 1.0e-9f) {
            const float s = v_max / v_mag;
            vd_ref *= s;
            vq_ref *= s;
        }

        simulation::AlphaBeta u_ref;
        simulation::inverseParkDqToAlphaBeta({vd_ref, vq_ref}, in.theta_e, u_ref);

        simulation::SwitchingState best_state = simulation::SwitchingState::S100;
        float best_dist = std::numeric_limits<float>::infinity();
        for (const auto state : simulation::kAllSwitchingStates) {
            if (state == simulation::SwitchingState::S000 || state == simulation::SwitchingState::S111) {
                continue;
            }
            const simulation::AlphaBeta u = simulation::voltageAlphaBetaFromState(in.vdc, state);
            const float dist = (u.alpha - u_ref.alpha) * (u.alpha - u_ref.alpha) +
                               (u.beta - u_ref.beta) * (u.beta - u_ref.beta);
            if (dist < best_dist) {
                best_dist = dist;
                best_state = state;
            }
        }

        const simulation::AlphaBeta u_opt = simulation::voltageAlphaBetaFromState(in.vdc, best_state);
        const float denom = u_opt.alpha * u_opt.alpha + u_opt.beta * u_opt.beta;
        float d_opt =
            (denom > 1.0e-12f) ? (u_ref.alpha * u_opt.alpha + u_ref.beta * u_opt.beta) / denom : 0.0f;
        d_opt = std::clamp(d_opt, 0.0f, 1.0f);

        const float valpha = d_opt * u_opt.alpha;
        const float vbeta = d_opt * u_opt.beta;
        simulation::Dq vdq;
        simulation::parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);

        const float id_pred = in.id + ts * (f_d + b0_d * vdq.d);
        const float iq_pred = in.iq + ts * (f_q + b0_q * vdq.q);

        const auto cmd = simulation::switchingStateToCommand(best_state);
        out.sa = cmd.sa;
        out.sb = cmd.sb;
        out.sc = cmd.sc;
        out.switching_state = best_state;
        out.valpha = valpha;
        out.vbeta = vbeta;
        out.vd = vdq.d;
        out.vq = vdq.q;
        out.duty_active = d_opt;
        out.predicted_id = id_pred;
        out.predicted_iq = iq_pred;
        out.min_cost = normalizedCost(in.id_ref, in.iq_ref, id_pred, iq_pred, params_.i_base);
        out.valid = true;

        prev_cmd_ = cmd;
        vd_prev_ = vdq.d;
        vq_prev_ = vdq.q;
        return out;
    }

    static float normalizedCost(float id_ref, float iq_ref, float id_pred, float iq_pred, float i_base) {
        const float base = (i_base > 0.0f) ? i_base : 1.0f;
        const float ed = (id_ref - id_pred) / base;
        const float eq = (iq_ref - iq_pred) / base;
        return ed * ed + eq * eq;
    }

private:
    void configureEsos() {
        LinearAdrcParams pd;
        pd.dt = params_.ts;
        pd.b0 = (params_.motor.ld > 0.0f) ? (1.0f / params_.motor.ld) : 10000.0f;
        pd.omega_o = params_.omega_o;
        pd.omega_c = params_.omega_o / 3.0f;
        eso_d_.params() = pd;

        LinearAdrcParams pq = pd;
        pq.b0 = (params_.motor.lq > 0.0f) ? (1.0f / params_.motor.lq) : 10000.0f;
        eso_q_.params() = pq;
    }

    bool validate(const HybridInputs& in) const {
        if (!(params_.ts > 0.0f) || params_.motor.ld <= 0.0f || params_.motor.lq <= 0.0f) {
            return false;
        }
        if (!(in.vdc > 0.0f)) {
            return false;
        }
        const auto ok = [](float x) { return std::isfinite(x); };
        return ok(in.id) && ok(in.iq) && ok(in.id_ref) && ok(in.iq_ref) && ok(in.theta_e);
    }

    HybridParams params_;
    LinearAdrc eso_d_;
    LinearAdrc eso_q_;
    simulation::SwitchCommand prev_cmd_;
    float vd_prev_ = 0.0f;
    float vq_prev_ = 0.0f;
    bool initialized_ = false;
};

}  // namespace hybrid_adrc_mpcc
