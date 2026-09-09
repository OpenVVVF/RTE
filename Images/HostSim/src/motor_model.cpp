#include "motor_model.h"

#include <algorithm>

namespace hostsim {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

float WrapAngle(float theta) {
    while (theta >= kTwoPi) theta -= kTwoPi;
    while (theta < 0.0f) theta += kTwoPi;
    return theta;
}
} // namespace

void MotorModel::SetParams(const MotorParams& params) {
    params_ = params;
}

void MotorModel::Reset() {
    state_ = MotorState{};
}

float MotorModel::ClampDuty(float duty_pct) {
    return std::max(0.0f, std::min(100.0f, duty_pct));
}

void MotorModel::DutiesToAbcVoltage(float du, float dv, float dw, float vdc,
                                    float* va, float* vb, float* vc) {
    const float scale = vdc / 100.0f;
    if (va) *va = ClampDuty(du) * scale;
    if (vb) *vb = ClampDuty(dv) * scale;
    if (vc) *vc = ClampDuty(dw) * scale;
}

void MotorModel::AbcToDq(float va, float vb, float vc, float theta,
                         float* vd, float* vq) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    const float v_alpha = (2.0f / 3.0f) * (va - 0.5f * vb - 0.5f * vc);
    const float v_beta = (2.0f / 3.0f) * (0.8660254f * vb - 0.8660254f * vc);
    if (vd) *vd = v_alpha * c + v_beta * s;
    if (vq) *vq = -v_alpha * s + v_beta * c;
}

void MotorModel::DqToAbc(float id, float iq, float theta,
                         float* ia, float* ib, float* ic) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    const float i_alpha = id * c - iq * s;
    const float i_beta = id * s + iq * c;
    if (ia) *ia = i_alpha;
    if (ib) *ib = -0.5f * i_alpha + 0.8660254f * i_beta;
    if (ic) *ic = -0.5f * i_alpha - 0.8660254f * i_beta;
}

void MotorModel::Step(float duty_u_pct, float duty_v_pct, float duty_w_pct,
                      float dt_s) {
    if (dt_s <= 0.0f) return;

    float va = 0.0f;
    float vb = 0.0f;
    float vc = 0.0f;
    DutiesToAbcVoltage(duty_u_pct, duty_v_pct, duty_w_pct, params_.vdc_v,
                       &va, &vb, &vc);

    float vd = 0.0f;
    float vq = 0.0f;
    AbcToDq(va, vb, vc, state_.theta_e_rad, &vd, &vq);

    const float omega = state_.omega_e_rad_s;
    const float did = (vd - params_.rs_ohm * state_.id_a +
                       omega * params_.lq_h * state_.iq_a) /
                      params_.ld_h;
    const float diq = (vq - params_.rs_ohm * state_.iq_a -
                       omega * (params_.ld_h * state_.id_a + params_.flux_wb)) /
                      params_.lq_h;

    state_.id_a += did * dt_s;
    state_.iq_a += diq * dt_s;

    const float torque =
        1.5f * static_cast<float>(params_.pole_pairs) *
        (params_.flux_wb * state_.iq_a +
         (params_.ld_h - params_.lq_h) * state_.id_a * state_.iq_a);
    const float friction = params_.friction_nm_per_rad_s * omega;
    const float domega = (torque - friction) / params_.inertia_kg_m2;
    state_.omega_e_rad_s += domega * dt_s;
    state_.theta_e_rad = WrapAngle(state_.theta_e_rad + omega * dt_s);

    DqToAbc(state_.id_a, state_.iq_a, state_.theta_e_rad,
            &state_.ia_a, &state_.ib_a, &state_.ic_a);
}

float MotorModel::ThetaElectricalDeg() const {
    return state_.theta_e_rad * 180.0f / kPi;
}

} // namespace hostsim
