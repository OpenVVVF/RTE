#include "pwm_scope.h"

#include <algorithm>
#include <cmath>

namespace hostsim {

namespace {
PwmScope g_pwm_scope;
} // namespace

PwmScope& GlobalPwmScope() { return g_pwm_scope; }

void PwmScope::SetDuties(float duty_u_pct, float duty_v_pct, float duty_w_pct) {
    duty_u_ = std::clamp(duty_u_pct, 0.0f, 100.0f);
    duty_v_ = std::clamp(duty_v_pct, 0.0f, 100.0f);
    duty_w_ = std::clamp(duty_w_pct, 0.0f, 100.0f);
}

void PwmScope::AdvanceOnce(float dt_s) {
    if (carrier_hz_ <= 0.0f || dt_s <= 0.0f) {
        gate_u_ = gate_v_ = gate_w_ = 0.0f;
        v_u_ = v_v_ = v_w_ = 0.0f;
        v_uv_ = v_vw_ = v_wu_ = 0.0f;
        return;
    }

    phase_ += carrier_hz_ * dt_s;
    phase_ -= std::floor(phase_);

    const float tri = 1.0f - std::fabs(2.0f * phase_ - 1.0f);
    const float du = duty_u_ * 0.01f;
    const float dv = duty_v_ * 0.01f;
    const float dw = duty_w_ * 0.01f;

    gate_u_ = du > tri ? 1.0f : 0.0f;
    gate_v_ = dv > tri ? 1.0f : 0.0f;
    gate_w_ = dw > tri ? 1.0f : 0.0f;

    v_u_ = gate_u_ * vdc_;
    v_v_ = gate_v_ * vdc_;
    v_w_ = gate_w_ * vdc_;
    v_uv_ = v_u_ - v_v_;
    v_vw_ = v_v_ - v_w_;
    v_wu_ = v_w_ - v_u_;
}

void PwmScope::AdvanceInterval(float dt_s) {
    if (carrier_hz_ <= 0.0f || dt_s <= 0.0f) {
        AdvanceOnce(0.0f);
        return;
    }

    /* At least ~8 samples per PWM period so edges land in the right place. */
    const int steps =
        std::max(1, static_cast<int>(std::ceil(dt_s * carrier_hz_ * 8.0f)));
    const float sub_dt = dt_s / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        AdvanceOnce(sub_dt);
    }
}

} // namespace hostsim
