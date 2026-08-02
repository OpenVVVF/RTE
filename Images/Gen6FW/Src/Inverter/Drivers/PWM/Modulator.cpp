/**
 * @file    Modulator.cpp
 * @brief   Modulator slot registry + the async modulators (SVPWM, SPWM).
 *
 * The math below is the exact code moved out of pwm.cpp (step 1 of the
 * multi-modulator plan: wrap existing modulation behind the Modulator
 * interface with zero behavioral change).
 */

#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/pwm.h"

#include <cmath>

namespace Inverter {
namespace {

constexpr float kTwoPi = 6.283185307f;

/* SVPWM linear over-modulation limit: 2/sqrt(3) */
constexpr float kSvpwmMMax = 1.154700538f;

float clampDuty(float d) {
    if (d < 0.0f) return 0.0f;
    if (d > 100.0f) return 100.0f;
    return d;
}

/**
 * @brief SVPWM: stationary-frame voltage vector -> three-phase duties.
 *
 * Stateless aside from the computed duties.  Externally clocked: the control
 * loop (FOC hook, generated tim_isr domain) calls update() + commit() each
 * control period.
 */
class SvpwmModulator final : public Modulator {
public:
    const char* name() const override { return "svpwm"; }
    bool runsInPwmIsr() const override { return false; }

    bool enter(float /*theta_e_rad*/, float /*modulation_index*/) override { return true; }
    void exit() override {}

    void update(float valpha_v, float vbeta_v, float vdc_v) override {
        if (vdc_v <= 1.0f) {
            m_du = 50.0f;
            m_dv = 50.0f;
            m_dw = 50.0f;
            return;
        }

        /* Clamp the alpha/beta magnitude to the linear modulation limit before
         * converting to three-phase voltages.  The SVPWM linear limit is
         * Vdc / sqrt(3). */
        const float sqrt3 = 1.7320508075688772f;
        float valpha = valpha_v;
        float vbeta  = vbeta_v;
        const float v_max_linear = (vdc_v / sqrt3) * 0.95f;
        const float v_albe_sq = valpha * valpha + vbeta * vbeta;
        if (v_albe_sq > v_max_linear * v_max_linear && v_albe_sq > 1e-12f) {
            const float scale = v_max_linear / std::sqrt(v_albe_sq);
            valpha *= scale;
            vbeta  *= scale;
        }

        /* Inverse Clarke: alpha/beta -> A/B/C. */
        const float va = valpha;
        const float vb = -0.5f * valpha + 0.5f * sqrt3 * vbeta;
        const float vc = -0.5f * valpha - 0.5f * sqrt3 * vbeta;

        /* Min-max SVPWM zero-sequence injection. */
        const float v_max = (va > vb) ? ((va > vc) ? va : vc) : ((vb > vc) ? vb : vc);
        const float v_min = (va < vb) ? ((va < vc) ? va : vc) : ((vb < vc) ? vb : vc);
        const float vcom = 0.5f * (v_max + v_min);

        m_du = clampDuty(50.0f + 50.0f * (va - vcom) / vdc_v);
        m_dv = clampDuty(50.0f + 50.0f * (vb - vcom) / vdc_v);
        m_dw = clampDuty(50.0f + 50.0f * (vc - vcom) / vdc_v);
    }

    void commit() override {
        PWM_SetThreePhaseDuty(m_du, m_dv, m_dw);
    }

private:
    float m_du = 50.0f;
    float m_dv = 50.0f;
    float m_dw = 50.0f;
};

/**
 * @brief Open-loop SPWM ramp with min-max zero-sequence injection.
 *
 * Self-clocked: the TIM1 update ISR drives update() + commit() while this
 * modulator owns the slot.  Params arrive via spwmSetParams() (legacy
 * PWM_SetSPWMParams flow); enter() restarts the ramp from angle zero.
 */
class SpwmModulator final : public Modulator {
public:
    const char* name() const override { return "spwm"; }
    bool runsInPwmIsr() const override { return true; }

    bool enter(float /*theta_e_rad*/, float /*modulation_index*/) override {
        m_angle = 0.0f;
        m_running = true;
        return true;
    }

    void exit() override {
        m_running = false;
    }

    void setParams(float fundamental_freq_hz, float modulation_index) {
        if (fundamental_freq_hz < 0.0f) fundamental_freq_hz = 0.0f;
        if (modulation_index < 0.0f) modulation_index = 0.0f;
        if (modulation_index > kSvpwmMMax) modulation_index = kSvpwmMMax;
        m_fundamental_freq_hz = fundamental_freq_hz;
        m_modulation_index = modulation_index;
    }

    void update(float /*valpha_v*/, float /*vbeta_v*/, float /*vdc_v*/) override {
        float angle = m_angle;
        const float m = m_modulation_index;

        /* Three-phase sinusoidal references, 120 deg apart. */
        const float u = m * sinf(angle);
        const float v = m * sinf(angle - kTwoPi / 3.0f);
        const float w = m * sinf(angle + kTwoPi / 3.0f);

        /* Min-max SVPWM zero-sequence injection to extend linear range to 2/sqrt(3). */
        const float v_max = (u > v) ? ((u > w) ? u : w) : ((v > w) ? v : w);
        const float v_min = (u < v) ? ((u < w) ? u : w) : ((v < w) ? v : w);
        const float v0 = -0.5f * (v_max + v_min);

        /* Convert to centered duty cycles [0, 100]. */
        m_du = clampDuty(50.0f + 50.0f * (u + v0));
        m_dv = clampDuty(50.0f + 50.0f * (v + v0));
        m_dw = clampDuty(50.0f + 50.0f * (w + v0));

        /* Advance angle by one PWM period. */
        angle += kTwoPi * m_fundamental_freq_hz / PWM_GetFrequency();
        if (angle >= kTwoPi) {
            angle -= kTwoPi;
            ++m_elec_cycles;
        }
        m_angle = angle;
    }

    void commit() override {
        PWM_SetThreePhaseDuty(m_du, m_dv, m_dw);
    }

    bool isRunning() const { return m_running; }
    float angleRad() const { return m_running ? m_angle : 0.0f; }
    void setAngle(float angle_rad) {
        while (angle_rad < 0.0f) angle_rad += kTwoPi;
        while (angle_rad >= kTwoPi) angle_rad -= kTwoPi;
        m_angle = angle_rad;
    }
    uint32_t elecCycles() const { return m_elec_cycles; }
    void resetElecCycles() { m_elec_cycles = 0; }

    float fundamentalFreqHz() const { return m_fundamental_freq_hz; }
    float modulationIndex() const { return m_modulation_index; }

private:
    float m_angle = 0.0f;
    float m_fundamental_freq_hz = 1.0f;
    float m_modulation_index = 0.0f;
    float m_du = 50.0f;
    float m_dv = 50.0f;
    float m_dw = 50.0f;
    uint32_t m_elec_cycles = 0;
    bool m_running = false;
};

SvpwmModulator s_svpwm;
SpwmModulator s_spwm;
Modulator* s_active = nullptr;

} // namespace

Modulator* activeModulator() { return s_active; }
void setActiveModulator(Modulator* m) { s_active = m; }

Modulator& svpwmModulator() { return s_svpwm; }
Modulator& spwmModulator() { return s_spwm; }

bool spwmIsRunning() { return s_spwm.isRunning(); }
float spwmAngleRad() { return s_spwm.angleRad(); }
uint32_t spwmElectricalCycles() { return s_spwm.elecCycles(); }
void spwmResetElectricalCycles() { s_spwm.resetElecCycles(); }
void spwmSetParams(float fundamental_freq_hz, float modulation_index) {
    s_spwm.setParams(fundamental_freq_hz, modulation_index);
}
float spwmFundamentalFreqHz() { return s_spwm.fundamentalFreqHz(); }
float spwmModulationIndex() { return s_spwm.modulationIndex(); }
void spwmSetAngle(float angle_rad) { s_spwm.setAngle(angle_rad); }

} // namespace Inverter
