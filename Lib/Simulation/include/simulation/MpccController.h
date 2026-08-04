#pragma once

/**
 * @file MpccController.h
 * @brief Finite-control-set model-predictive current control (FCS-MPCC)
 *        for a three-phase PMSM / two-level VSI.
 *
 * Technical reference (improved MPCC modes):
 * Y. Zhang, D. Xu, J. Liu, S. Gao, and W. Xu,
 * "Performance Improvement of Model-Predictive Current Control
 * of Permanent Magnet Synchronous Motor Drives,"
 * IEEE Transactions on Industry Applications,
 * vol. 53, no. 4, pp. 3683-3695, July/August 2017.
 * DOI: 10.1109/TIA.2017.2690998.
 *
 * Prediction and cost evaluation are implemented in the synchronous dq
 * frame (project Stage 6). Optimal-duty mode follows Zhang Method II
 * (active + zero vector) but with a consistent dq deadbeat voltage and
 * per-vector duty search — the previous αβ/dq mix was incorrect and
 * unstable.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "simulation/PmsmPlant.h"
#include "simulation/TwoLevelInverter.h"
#include "simulation/Transforms.h"

namespace simulation {

enum class MPCCMode {
    ConventionalOneStep,  // one discrete vector per sample
    DelayCompensated,     // Heun one-step delay compensation in dq
    BackEMFCompensated,   // delay compensation + model cross-coupling EMF
    OptimalDutyCycle      // Method II: best active vector + duty with zero vector
};

struct MpccParameters {
    PmsmParameters motor;
    float ts = 100e-6f;
    float i_base = 10.0f;
    float i_max = 30.0f;
    float current_limit_penalty = 1.0e6f;
    float cost_tie_tolerance = 1.0e-9f;
    MPCCMode mode = MPCCMode::ConventionalOneStep;
};

struct MpccInputs {
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float vdc = 540.0f;
    bool enable = true;
};

struct MpccOutputs {
    bool sa = false;
    bool sb = false;
    bool sc = false;
    SwitchingState switching_state = SwitchingState::S000;
    float predicted_id = 0.0f;
    float predicted_iq = 0.0f;
    float min_cost = std::numeric_limits<float>::infinity();
    float valpha = 0.0f;
    float vbeta = 0.0f;
    float vd = 0.0f;
    float vq = 0.0f;
    float duty_active = 1.0f;
    bool valid = false;
};

struct MpccHistory {
    float vd_prev = 0.0f;
    float vq_prev = 0.0f;
    float valpha_prev = 0.0f;
    float vbeta_prev = 0.0f;
    bool initialized = false;
};

class MpccController {
public:
    explicit MpccController(MpccParameters params = {}) : params_(params), prev_cmd_({false, false, false}) {}

    const MpccParameters& parameters() const { return params_; }
    MpccParameters& parameters() { return params_; }

    void reset() {
        prev_cmd_ = {};
        history_ = {};
    }

    MpccOutputs update(const MpccInputs& in) {
        MpccOutputs out;
        if (!in.enable || !validateInputs(in)) {
            out.sa = prev_cmd_.sa;
            out.sb = prev_cmd_.sb;
            out.sc = prev_cmd_.sc;
            out.switching_state = commandToSwitchingState(prev_cmd_);
            return out;
        }

        switch (params_.mode) {
            case MPCCMode::ConventionalOneStep:
                out = evaluateConventional(in);
                break;
            case MPCCMode::DelayCompensated:
                out = evaluateDelayCompensated(in, /*use_model_emf=*/false);
                break;
            case MPCCMode::BackEMFCompensated:
                out = evaluateDelayCompensated(in, /*use_model_emf=*/true);
                break;
            case MPCCMode::OptimalDutyCycle:
                out = evaluateOptimalDuty(in);
                break;
        }

        if (out.valid) {
            prev_cmd_ = {out.sa, out.sb, out.sc};
            history_.vd_prev = out.vd;
            history_.vq_prev = out.vq;
            history_.valpha_prev = out.valpha;
            history_.vbeta_prev = out.vbeta;
            history_.initialized = true;
        }
        return out;
    }

    /** One-step forward-Euler dq current prediction. */
    static Dq predictDqCurrent(const PmsmParameters& motor, float ts, float id, float iq, float omega_e, float vd,
                               float vq) {
        if (!(ts > 0.0f) || motor.ld <= 0.0f || motor.lq <= 0.0f) {
            return {id, iq};
        }
        Dq next;
        next.d = id + (ts / motor.ld) * (vd - motor.rs * id + omega_e * motor.lq * iq);
        next.q = iq + (ts / motor.lq) * (vq - motor.rs * iq - omega_e * motor.ld * id - omega_e * motor.psi_f);
        return next;
    }

    static float normalizedCost(float id_ref, float iq_ref, float id_pred, float iq_pred, float i_base) {
        const float base = (i_base > 0.0f) ? i_base : 1.0f;
        const float ed = (id_ref - id_pred) / base;
        const float eq = (iq_ref - iq_pred) / base;
        return ed * ed + eq * eq;
    }

private:
    bool validateInputs(const MpccInputs& in) const {
        if (!(params_.ts > 0.0f) || params_.motor.ld <= 0.0f || params_.motor.lq <= 0.0f) {
            return false;
        }
        if (!(in.vdc > 0.0f)) {
            return false;
        }
        const auto finite = [](float x) { return std::isfinite(x); };
        return finite(in.id) && finite(in.iq) && finite(in.id_ref) && finite(in.iq_ref) && finite(in.theta_e) &&
               finite(in.omega_e);
    }

    /** Model cross-coupling / PM voltage treated as known disturbance (dq). */
    static void modelEmfDq(const PmsmParameters& m, float id, float iq, float omega_e, float& ed, float& eq) {
        ed = -omega_e * m.lq * iq;
        eq = omega_e * m.ld * id + omega_e * m.psi_f;
    }

    /**
     * Heun-style one-step delay compensation in dq using the previously
     * applied voltage (Zhang Sec. III-B idea, dq-consistent).
     */
    Dq delayCompensatedDq(const MpccInputs& in, bool use_model_emf) const {
        if (!history_.initialized) {
            return {in.id, in.iq};
        }
        const auto& m = params_.motor;
        const float ts = params_.ts;
        float ed = 0.0f;
        float eq = 0.0f;
        if (use_model_emf) {
            modelEmfDq(m, in.id, in.iq, in.omega_e, ed, eq);
        } else {
            // Use model EMF even in DelayCompensated for physical consistency;
            // without it the predictor ignores rotation/PM terms.
            modelEmfDq(m, in.id, in.iq, in.omega_e, ed, eq);
        }

        const float did_sp = (history_.vd_prev - m.rs * in.id - ed) / m.ld;
        const float diq_sp = (history_.vq_prev - m.rs * in.iq - eq) / m.lq;
        const float id_sp = in.id + ts * did_sp;
        const float iq_sp = in.iq + ts * diq_sp;

        // Heun correction on resistive drop (Zhang eq. 11-12 style).
        const float id_corr = id_sp + (-m.rs * (id_sp - in.id) * ts) / (2.0f * m.ld);
        const float iq_corr = iq_sp + (-m.rs * (iq_sp - in.iq) * ts) / (2.0f * m.lq);
        return {id_corr, iq_corr};
    }

    MpccOutputs evaluateConventional(const MpccInputs& in) {
        return evaluateFcsOverStates(in, in.id, in.iq, 1.0f);
    }

    MpccOutputs evaluateDelayCompensated(const MpccInputs& in, bool use_model_emf) {
        const Dq i0 = delayCompensatedDq(in, use_model_emf);
        return evaluateFcsOverStates(in, i0.d, i0.q, 1.0f);
    }

    /**
     * Zhang Method II (dq-consistent, synchronous sample — no extra delay):
     * deadbeat v_dq* → αβ → nearest active vector → duty with zero-vector.
     * Delay compensation is omitted because this plant applies u(k) with i(k).
     */
    MpccOutputs evaluateOptimalDuty(const MpccInputs& in) {
        const auto& m = params_.motor;
        const float ts = params_.ts;

        const float vd_ref =
            m.rs * in.id + (m.ld / ts) * (in.id_ref - in.id) - in.omega_e * m.lq * in.iq;
        const float vq_ref = m.rs * in.iq + (m.lq / ts) * (in.iq_ref - in.iq) +
                             in.omega_e * m.ld * in.id + in.omega_e * m.psi_f;

        // Voltage limit (inscribed circle) to keep duty meaningful near base speed.
        const float v_max = (in.vdc / kSqrt3) * 0.95f;
        const float v_mag = std::sqrt(vd_ref * vd_ref + vq_ref * vq_ref);
        float vd_c = vd_ref;
        float vq_c = vq_ref;
        if (v_mag > v_max && v_mag > 1.0e-9f) {
            const float s = v_max / v_mag;
            vd_c *= s;
            vq_c *= s;
        }

        AlphaBeta u_ref;
        inverseParkDqToAlphaBeta({vd_c, vq_c}, in.theta_e, u_ref);

        SwitchingState best_state = SwitchingState::S100;
        float best_dist = std::numeric_limits<float>::infinity();
        for (const SwitchingState state : kAllSwitchingStates) {
            if (state == SwitchingState::S000 || state == SwitchingState::S111) {
                continue;
            }
            const AlphaBeta u = voltageAlphaBetaFromState(in.vdc, state);
            const float dist = (u.alpha - u_ref.alpha) * (u.alpha - u_ref.alpha) +
                               (u.beta - u_ref.beta) * (u.beta - u_ref.beta);
            if (dist < best_dist) {
                best_dist = dist;
                best_state = state;
            }
        }

        const AlphaBeta u_opt = voltageAlphaBetaFromState(in.vdc, best_state);
        const float denom = u_opt.alpha * u_opt.alpha + u_opt.beta * u_opt.beta;
        float d_opt = (denom > 1.0e-12f) ? (u_ref.alpha * u_opt.alpha + u_ref.beta * u_opt.beta) / denom : 0.0f;
        d_opt = std::clamp(d_opt, 0.0f, 1.0f);

        const float valpha = d_opt * u_opt.alpha;
        const float vbeta = d_opt * u_opt.beta;
        Dq vdq;
        parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);
        const Dq pred = predictDqCurrent(m, ts, in.id, in.iq, in.omega_e, vdq.d, vdq.q);

        const SwitchCommand cmd = switchingStateToCommand(best_state);
        MpccOutputs out;
        out.sa = cmd.sa;
        out.sb = cmd.sb;
        out.sc = cmd.sc;
        out.switching_state = best_state;
        out.valpha = valpha;
        out.vbeta = vbeta;
        out.vd = vdq.d;
        out.vq = vdq.q;
        out.duty_active = d_opt;
        out.predicted_id = pred.d;
        out.predicted_iq = pred.q;
        out.min_cost = normalizedCost(in.id_ref, in.iq_ref, pred.d, pred.q, params_.i_base);
        out.valid = true;
        return out;
    }

    MpccOutputs evaluateFcsOverStates(const MpccInputs& in, float id0, float iq0, float duty) {
        MpccOutputs best;
        best.min_cost = std::numeric_limits<float>::infinity();

        for (const SwitchingState state : kAllSwitchingStates) {
            const AlphaBeta v_ab = voltageAlphaBetaFromState(in.vdc, state);
            const float valpha = v_ab.alpha * duty;
            const float vbeta = v_ab.beta * duty;

            Dq vdq;
            parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);

            const Dq pred =
                predictDqCurrent(params_.motor, params_.ts, id0, iq0, in.omega_e, vdq.d, vdq.q);

            float cost = normalizedCost(in.id_ref, in.iq_ref, pred.d, pred.q, params_.i_base);
            const float i_mag = std::sqrt(pred.d * pred.d + pred.q * pred.q);
            if (i_mag > params_.i_max) {
                cost += params_.current_limit_penalty;
            }

            const SwitchCommand candidate = switchingStateToCommand(state);
            if (cost < best.min_cost - params_.cost_tie_tolerance) {
                best.min_cost = cost;
                best.sa = candidate.sa;
                best.sb = candidate.sb;
                best.sc = candidate.sc;
                best.switching_state = state;
                best.predicted_id = pred.d;
                best.predicted_iq = pred.q;
                best.valpha = valpha;
                best.vbeta = vbeta;
                best.duty_active = duty;
                best.vd = vdq.d;
                best.vq = vdq.q;
                best.valid = true;
            } else if (std::abs(cost - best.min_cost) <= params_.cost_tie_tolerance && best.valid) {
                const int cand_trans = countSwitchTransitions(prev_cmd_, candidate);
                const SwitchCommand best_cmd{best.sa, best.sb, best.sc};
                const int best_trans = countSwitchTransitions(prev_cmd_, best_cmd);
                const bool same_as_prev =
                    candidate.sa == prev_cmd_.sa && candidate.sb == prev_cmd_.sb && candidate.sc == prev_cmd_.sc;
                const bool best_same = best_cmd.sa == prev_cmd_.sa && best_cmd.sb == prev_cmd_.sb &&
                                       best_cmd.sc == prev_cmd_.sc;

                if (cand_trans < best_trans || (cand_trans == best_trans && same_as_prev && !best_same) ||
                    (cand_trans == best_trans && same_as_prev == best_same &&
                     static_cast<int>(state) < static_cast<int>(best.switching_state))) {
                    best.sa = candidate.sa;
                    best.sb = candidate.sb;
                    best.sc = candidate.sc;
                    best.switching_state = state;
                    best.predicted_id = pred.d;
                    best.predicted_iq = pred.q;
                    best.valpha = valpha;
                    best.vbeta = vbeta;
                    best.duty_active = duty;
                    best.vd = vdq.d;
                    best.vq = vdq.q;
                }
            }
        }

        return best;
    }

    MpccParameters params_;
    SwitchCommand prev_cmd_;
    MpccHistory history_;
};

}  // namespace simulation
