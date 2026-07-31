#pragma once

/**
 * @file MpccController.h
 * @brief Model-predictive current controller for a three-phase
 *        PMSM supplied by a two-level voltage-source inverter.
 *
 * Technical reference:
 * Y. Zhang, D. Xu, J. Liu, S. Gao, and W. Xu,
 * "Performance Improvement of Model-Predictive Current Control
 * of Permanent Magnet Synchronous Motor Drives,"
 * IEEE Transactions on Industry Applications,
 * vol. 53, no. 4, pp. 3683-3695, July/August 2017.
 * DOI: 10.1109/TIA.2017.2690998.
 *
 * This C++ implementation was independently developed as a new
 * controller node for the RTE simulation framework.
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

/** Zhang et al. (2017) improved MPCC modes — selectable after baseline FCS-MPCC. */
enum class MPCCMode {
    ConventionalOneStep,   // Stage 6 baseline
    DelayCompensated,      // Paper Sec. III-B, eq. (11)-(12) Heun delay compensation
    BackEMFCompensated,    // Paper Sec. III-A, eq. (7)-(10) EMF estimation in prediction
    OptimalDutyCycle,      // Paper Sec. III-C Method II, eq. (18)-(19)
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
    float duty_active = 1.0f;  // 1.0 for conventional one-step; [0,1] for optimal duty
    bool valid = false;
};

struct MpccHistory {
    float u_alpha_prev = 0.0f;
    float u_beta_prev = 0.0f;
    float id_prev = 0.0f;
    float iq_prev = 0.0f;
    float ex_alpha = 0.0f;
    float ex_beta = 0.0f;
    std::array<float, 3> ex_alpha_hist = {};
    std::array<float, 3> ex_beta_hist = {};
    int hist_index = 0;
    bool initialized = false;
};

class MpccController {
public:
    explicit MpccController(MpccParameters params = {}) : params_(params), prev_cmd_({false, false, false}) {}

    const MpccParameters& parameters() const { return params_; }
    MpccParameters& parameters() { return params_; }

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
                out = evaluateDelayCompensated(in);
                break;
            case MPCCMode::BackEMFCompensated:
                out = evaluateBackEmfCompensated(in);
                break;
            case MPCCMode::OptimalDutyCycle:
                out = evaluateOptimalDuty(in);
                break;
        }

        if (out.valid) {
            prev_cmd_ = {out.sa, out.sb, out.sc};
            updateHistory(in, out);
        }
        return out;
    }

    /** One-step dq current prediction — paper Stage 6 / eq. discretized dq model. */
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

    Dq compensatedCurrentConventional(const MpccInputs& in) const {
        return {in.id, in.iq};
    }

    /** Paper eq. (11)-(12): Heun delay compensation to obtain i(k+1). */
    // Delay compensation follows the formulation in
    // Zhang et al., IEEE Transactions on Industry Applications, 2017.
    // DOI: 10.1109/TIA.2017.2690998.
    Dq delayCompensatedCurrent(const MpccInputs& in, float ex_alpha, float ex_beta) const {
        const float lq = params_.motor.lq;
        const float ts = params_.ts;
        const float rs = params_.motor.rs;

        const float u_alpha = history_.u_alpha_prev;
        const float u_beta = history_.u_beta_prev;

        const float di_alpha_sp = (u_alpha - rs * in.id - ex_alpha) / lq;
        const float di_beta_sp = (u_beta - rs * in.iq - ex_beta) / lq;

        const float id_sp = in.id + ts * di_alpha_sp;
        const float iq_sp = in.iq + ts * di_beta_sp;

        const float id_corr = id_sp + (-rs * (id_sp - in.id) * ts) / (2.0f * lq);
        const float iq_corr = iq_sp + (-rs * (iq_sp - in.iq) * ts) / (2.0f * lq);
        return {id_corr, iq_corr};
    }

    /** Paper eq. (7): EMF estimate from past voltage/current (alpha component form). */
    // Back-EMF estimation follows the formulation in
    // Zhang et al., IEEE Transactions on Industry Applications, 2017.
    // DOI: 10.1109/TIA.2017.2690998.
    static float estimateEmfComponent(float u_prev, float i_now, float i_prev, float rs, float lq, float ts) {
        return u_prev - rs * 0.5f * (i_now + i_prev) - (lq / ts) * (i_now - i_prev);
    }

    void updateHistory(const MpccInputs& in, const MpccOutputs& out) {
        history_.u_alpha_prev = out.valpha;
        history_.u_beta_prev = out.vbeta;
        history_.id_prev = in.id;
        history_.iq_prev = in.iq;

        if (params_.mode == MPCCMode::BackEMFCompensated || params_.mode == MPCCMode::OptimalDutyCycle) {
            const float ex_a =
                estimateEmfComponent(history_.u_alpha_prev, in.id, history_.id_prev, params_.motor.rs,
                                     params_.motor.lq, params_.ts);
            const float ex_b =
                estimateEmfComponent(history_.u_beta_prev, in.iq, history_.iq_prev, params_.motor.rs,
                                     params_.motor.lq, params_.ts);
            history_.ex_alpha_hist[static_cast<std::size_t>(history_.hist_index)] = ex_a;
            history_.ex_beta_hist[static_cast<std::size_t>(history_.hist_index)] = ex_b;
            history_.hist_index = (history_.hist_index + 1) % 3;
            history_.ex_alpha = (history_.ex_alpha_hist[0] + history_.ex_alpha_hist[1] + history_.ex_alpha_hist[2]) / 3.0f;
            history_.ex_beta = (history_.ex_beta_hist[0] + history_.ex_beta_hist[1] + history_.ex_beta_hist[2]) / 3.0f;
        }
        history_.initialized = true;
    }

    MpccOutputs evaluateConventional(const MpccInputs& in) {
        const Dq i_base = compensatedCurrentConventional(in);
        return evaluateFcsOverStates(in, i_base.d, i_base.q, i_base.d, i_base.q, 1.0f);
    }

    MpccOutputs evaluateDelayCompensated(const MpccInputs& in) {
        const Dq i_kp1 = delayCompensatedCurrent(in, history_.ex_alpha, history_.ex_beta);
        return evaluateFcsOverStates(in, i_kp1.d, i_kp1.q, in.id, in.iq, 1.0f);
    }

    MpccOutputs evaluateBackEmfCompensated(const MpccInputs& in) {
        const Dq i_kp1 = delayCompensatedCurrent(in, history_.ex_alpha, history_.ex_beta);
        return evaluateFcsOverStates(in, i_kp1.d, i_kp1.q, in.id, in.iq, 1.0f);
    }

    // Optimal duty-cycle calculation follows the formulation in
    // Zhang et al., IEEE Transactions on Industry Applications, 2017.
    // DOI: 10.1109/TIA.2017.2690998.
    MpccOutputs evaluateOptimalDuty(const MpccInputs& in) {
        MpccOutputs out;
        const Dq i_kp1 = delayCompensatedCurrent(in, history_.ex_alpha, history_.ex_beta);

        // Paper eq. (18): deadbeat reference voltage in alpha-beta (stationary model).
        const float lq = params_.motor.lq;
        const float rs = params_.motor.rs;
        const float ts = params_.ts;

        const float u_ref_alpha =
            rs * i_kp1.d + lq * (in.id_ref - i_kp1.d) / ts + history_.ex_alpha;
        const float u_ref_beta =
            rs * i_kp1.q + lq * (in.iq_ref - i_kp1.q) / ts + history_.ex_beta;

        // Select nearest active vector by sector of u_ref.
        SwitchingState best_state = SwitchingState::S100;
        float best_dist = std::numeric_limits<float>::infinity();
        for (const SwitchingState state : kAllSwitchingStates) {
            if (state == SwitchingState::S000 || state == SwitchingState::S111) {
                continue;
            }
            const AlphaBeta v = voltageAlphaBetaFromState(in.vdc, state);
            const float dist = (v.alpha - u_ref_alpha) * (v.alpha - u_ref_alpha) + (v.beta - u_ref_beta) * (v.beta - u_ref_beta);
            if (dist < best_dist) {
                best_dist = dist;
                best_state = state;
            }
        }

        const AlphaBeta u_opt = voltageAlphaBetaFromState(in.vdc, best_state);
        // Paper eq. (19): optimal duty of active vector.
        float topt = (u_ref_alpha * u_opt.alpha + u_ref_beta * u_opt.beta) /
                     std::max(u_opt.alpha * u_opt.alpha + u_opt.beta * u_opt.beta, 1.0e-12f);
        topt = std::clamp(topt, 0.0f, 1.0f);

        const SwitchCommand cmd = switchingStateToCommand(best_state);
        out.sa = cmd.sa;
        out.sb = cmd.sb;
        out.sc = cmd.sc;
        out.switching_state = best_state;
        out.valpha = u_opt.alpha * topt;
        out.vbeta = u_opt.beta * topt;
        out.duty_active = topt;
        {
            Dq vdq;
            parkAlphaBetaToDq({out.valpha, out.vbeta}, in.theta_e, vdq);
            out.vd = vdq.d;
            out.vq = vdq.q;
        }
        out.predicted_id = in.id_ref;
        out.predicted_iq = in.iq_ref;
        out.min_cost = best_dist;
        out.valid = true;
        return out;
    }

    MpccOutputs evaluateFcsOverStates(const MpccInputs& in, float id_pred_base, float iq_pred_base,
                                       float id_for_cost, float iq_for_cost, float duty) {
        MpccOutputs best;
        best.min_cost = std::numeric_limits<float>::infinity();

        for (const SwitchingState state : kAllSwitchingStates) {
            const AlphaBeta v_ab = voltageAlphaBetaFromState(in.vdc, state);
            const float valpha = v_ab.alpha * duty;
            const float vbeta = v_ab.beta * duty;

            Dq vdq;
            parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);

            const Dq pred = predictDqCurrent(params_.motor, params_.ts, id_pred_base, iq_pred_base, in.omega_e,
                                           vdq.d, vdq.q);

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
                {
                    Dq vdq;
                    parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);
                    best.vd = vdq.d;
                    best.vq = vdq.q;
                }
                best.valid = true;
            } else if (std::abs(cost - best.min_cost) <= params_.cost_tie_tolerance && best.valid) {
                const int cand_trans = countSwitchTransitions(prev_cmd_, candidate);
                const SwitchCommand best_cmd{best.sa, best.sb, best.sc};
                const int best_trans = countSwitchTransitions(prev_cmd_, best_cmd);
                const bool same_as_prev =
                    candidate.sa == prev_cmd_.sa && candidate.sb == prev_cmd_.sb && candidate.sc == prev_cmd_.sc;
                const bool best_same = best_cmd.sa == prev_cmd_.sa && best_cmd.sb == prev_cmd_.sb &&
                                       best_cmd.sc == prev_cmd_.sc;

                if (cand_trans < best_trans ||
                    (cand_trans == best_trans && same_as_prev && !best_same) ||
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
                    {
                        Dq vdq;
                        parkAlphaBetaToDq({valpha, vbeta}, in.theta_e, vdq);
                        best.vd = vdq.d;
                        best.vq = vdq.q;
                    }
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
