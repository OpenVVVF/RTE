#pragma once

#include <cmath>
#include <cstdint>

#include "simulation/Transforms.h"

namespace simulation {

struct PmsmParameters {
    float rs = 2.2479f;
    float ld = 17.65e-3f;
    float lq = 17.65e-3f;
    float psi_f = 0.4686f;
    int pole_pairs = 4;
    float inertia = 0.0254f;
    float viscous_friction = 0.0f;
};

struct PmsmState {
    float id = 0.0f;
    float iq = 0.0f;
    float ia = 0.0f;
    float ib = 0.0f;
    float ic = 0.0f;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float theta_m = 0.0f;
    float omega_m = 0.0f;
    float torque_em = 0.0f;
};

/**
 * Discrete-time PMSM plant in the synchronous dq frame.
 * Standard equations (SPMSM/IPMSM):
 *   v_d = R i_d + L_d di_d/dt - omega_e L_q i_q
 *   v_q = R i_q + L_q di_q/dt + omega_e L_d i_d + omega_e psi_f
 *   T_e = (3/2) p [psi_f i_q + (L_d - L_q) i_d i_q]
 *   J d omega_m/dt = T_e - T_L - B omega_m
 *   omega_e = p omega_m
 */
class PmsmPlant {
public:
    explicit PmsmPlant(PmsmParameters params = {}) : params_(params) {}

    const PmsmParameters& parameters() const { return params_; }
    PmsmState& state() { return state_; }
    const PmsmState& state() const { return state_; }

    void reset(const PmsmState& initial = {}) {
        state_ = initial;
        syncAbcFromDq();
        const float p = static_cast<float>(params_.pole_pairs);
        state_.torque_em = 1.5f * p * (params_.psi_f * state_.iq + (params_.ld - params_.lq) * state_.id * state_.iq);
    }

    void step(float vd, float vq, float load_torque, float dt) {
        if (!(dt > 0.0f) || params_.ld <= 0.0f || params_.lq <= 0.0f) {
            return;
        }

        const float rs = params_.rs;
        const float ld = params_.ld;
        const float lq = params_.lq;
        const float psi_f = params_.psi_f;
        const float p = static_cast<float>(params_.pole_pairs);
        const float omega_e = state_.omega_e;

        const float did = (vd - rs * state_.id + omega_e * lq * state_.iq) / ld;
        const float diq = (vq - rs * state_.iq - omega_e * ld * state_.id - omega_e * psi_f) / lq;

        state_.id += did * dt;
        state_.iq += diq * dt;

        state_.torque_em = 1.5f * p * (psi_f * state_.iq + (ld - lq) * state_.id * state_.iq);

        const float domega_m =
            (state_.torque_em - load_torque - params_.viscous_friction * state_.omega_m) / params_.inertia;
        state_.omega_m += domega_m * dt;
        state_.omega_e = p * state_.omega_m;

        state_.theta_e = wrapAngle0TwoPi(state_.theta_e + state_.omega_e * dt);
        state_.theta_m = wrapAngle0TwoPi(state_.theta_m + state_.omega_m * dt);

        syncAbcFromDq();
    }

    void stepAlphaBeta(float valpha, float vbeta, float load_torque, float dt) {
        Dq vdq;
        parkAlphaBetaToDq({valpha, vbeta}, state_.theta_e, vdq);
        step(vdq.d, vdq.q, load_torque, dt);
    }

private:
    void syncAbcFromDq() {
        AlphaBeta i_ab;
        inverseParkDqToAlphaBeta({state_.id, state_.iq}, state_.theta_e, i_ab);
        Abc abc;
        inverseClarkeAlphaBetaToAbc(i_ab, abc);
        state_.ia = abc.a;
        state_.ib = abc.b;
        state_.ic = abc.c;
    }

    PmsmParameters params_;
    PmsmState state_;
};

}  // namespace simulation
