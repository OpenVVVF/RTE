#include "Inverter/Drivers/PWM/PatternModulator.h"

#include "main.h"
#include "tim.h"

#include <cmath>

namespace Inverter {

namespace {

/* TIM1 counter clock (same root as pwm.cpp: 275 MHz, PSC=0 in practice). */
constexpr float TIM1_CLK_HZ = 275000000.0f;

/* ---- SHE angle tables (offline Newton solve, verified: eliminated
 * harmonics < 1e-9 of the square-wave reference at every row) ---- */
struct SheRow {
    float m;
    float a[5];
};

/* N = 5 (eliminates 5th/7th/11th/13th; triplen content cancels in 3-wire). */
static const SheRow SHE5[] = {
    { 0.20f, { 0.02647817f, 0.32990860f, 0.72786141f, 1.01706629f, 1.42311273f }},
    { 0.30f, { 0.03996109f, 0.32068373f, 0.74255189f, 1.00203889f, 1.43654461f }},
    { 0.35f, { 0.04677178f, 0.31618964f, 0.74985756f, 0.99452409f, 1.44325874f }},
    { 0.40f, { 0.05363306f, 0.31178677f, 0.75713695f, 0.98700397f, 1.44997060f }},
    { 0.45f, { 0.06054903f, 0.30748513f, 0.76438927f, 0.97947459f, 1.45667947f }},
    { 0.50f, { 0.06752456f, 0.30329552f, 0.77161288f, 0.97193130f, 1.46338469f }},
    { 0.55f, { 0.07456541f, 0.29922973f, 0.77880499f, 0.96436848f, 1.47008566f }},
    { 0.60f, { 0.08167849f, 0.29530078f, 0.78596123f, 0.95677906f, 1.47678181f }},
    { 0.65f, { 0.08887202f, 0.29152321f, 0.79307500f, 0.94915385f, 1.48347258f }},
    { 0.70f, { 0.09615593f, 0.28791346f, 0.80013629f, 0.94148033f, 1.49015746f }},
    { 0.75f, { 0.10354226f, 0.28449030f, 0.80712978f, 0.93374072f, 1.49683597f }},
    { 0.80f, { 0.11104580f, 0.28127553f, 0.81403134f, 0.92590854f, 1.50350764f }},
    { 0.85f, { 0.11868488f, 0.27829485f, 0.82080131f, 0.91794182f, 1.51017211f }},
    { 0.90f, { 0.12648270f, 0.27557914f, 0.82737047f, 0.90976918f, 1.51682913f }},
    { 0.95f, { 0.13446930f, 0.27316654f, 0.83360739f, 0.90125734f, 1.52347877f }},
    { 1.00f, { 0.14268523f, 0.27110616f, 0.83923017f, 0.89212324f, 1.53012183f }},
    { 1.05f, { 0.15119039f, 0.26946678f, 0.84350796f, 0.88163719f, 1.53676140f }},
    { 1.10f, { 0.16009821f, 0.26837049f, 0.84378301f, 0.86715331f, 1.54341059f }},
    { 1.15f, { 0.16999099f, 0.26841182f, 0.81663086f, 0.82540251f, 1.55021123f }},
};
constexpr int SHE5_ROWS = sizeof(SHE5) / sizeof(SHE5[0]);
constexpr int SHE_N = 5;

/* Phase offsets for A/B/C (matches the inverse-Clarke convention). */
const float PHASE_OFFSET[3] = {0.0f, -2.09439510239f, 2.09439510239f};

float wrap2pi(float x) {
    x = std::fmod(x, 6.28318530718f);
    if (x < 0.0f) x += 6.28318530718f;
    return x;
}

/* OC mode register poke (avoids HAL reconfig of a running timer). */
void setOcMode(uint8_t phase, uint32_t mode) {
    switch (phase) {
        case 0:
            TIM1->CCMR1 = (TIM1->CCMR1 & ~TIM_CCMR1_OC1M) |
                          ((mode << TIM_CCMR1_OC1M_Pos) & TIM_CCMR1_OC1M);
            break;
        case 1:
            TIM1->CCMR1 = (TIM1->CCMR1 & ~TIM_CCMR1_OC2M) |
                          ((mode << TIM_CCMR1_OC2M_Pos) & TIM_CCMR1_OC2M);
            break;
        default:
            TIM1->CCMR2 = (TIM1->CCMR2 & ~TIM_CCMR2_OC3M) |
                          ((mode << TIM_CCMR2_OC3M_Pos) & TIM_CCMR2_OC3M);
            break;
    }
}

void writeCcr(uint8_t phase, uint32_t v) {
    switch (phase) {
        case 0: TIM1->CCR1 = v; break;
        case 1: TIM1->CCR2 = v; break;
        default: TIM1->CCR3 = v; break;
    }
}

} // namespace

PatternModulator& PatternModulator::instance() {
    static PatternModulator s_instance;
    return s_instance;
}

PatternModulator& patternModulator() {
    return PatternModulator::instance();
}

void PatternModulator::init() {
    m_enabled = false;
}

void PatternModulator::setCommand(float m, float delta_rad, float theta_e_rad,
                                  float omega_e_rad_s, int n_pulses) {
    (void)n_pulses;  /* v1 tables are N=5 only; parameter reserved. */
    m_cmd_m = m;
    m_cmd_delta = delta_rad;
    m_cmd_theta = theta_e_rad;
    m_cmd_omega = omega_e_rad_s;
    m_cmd_n = n_pulses;
}

void PatternModulator::sheAngles(float* out, int n) const {
    float m = m_cmd_m;
    if (m < SHE5[0].m) m = SHE5[0].m;
    if (m > SHE5[SHE5_ROWS - 1].m) m = SHE5[SHE5_ROWS - 1].m;
    int lo = 0;
    while (lo < SHE5_ROWS - 2 && SHE5[lo + 1].m < m) {
        ++lo;
    }
    const SheRow& a = SHE5[lo];
    const SheRow& b = SHE5[lo + 1];
    const float t = (m - a.m) / (b.m - a.m);
    for (int i = 0; i < n; ++i) {
        out[i] = a.a[i] + t * (b.a[i] - a.a[i]);
    }
}

float PatternModulator::waveLevel(float phi, float phase_offset) const {
    /* Reduce to the first half-cycle, then count toggle points below it. */
    float x = wrap2pi(phi + phase_offset);
    float sign = 1.0f;
    if (x >= 3.14159265359f) {
        x -= 3.14159265359f;
        sign = -1.0f;
    }
    float a[SHE_N];
    sheAngles(a, SHE_N);
    int toggles = 0;
    for (int i = 0; i < SHE_N; ++i) {
        if (a[i] <= x) ++toggles;
    }
    for (int i = SHE_N - 1; i >= 0; --i) {
        if ((3.14159265359f - a[i]) <= x) ++toggles;
    }
    return (toggles & 1) ? -sign : sign;
}

float PatternModulator::nextEdgeAngle(float phi, float phase_offset) const {
    float x = wrap2pi(phi + phase_offset);
    float a[SHE_N];
    sheAngles(a, SHE_N);
    float best = 1.0e30f;
    for (int i = 0; i < SHE_N; ++i) {
        const float cands[4] = {a[i], 3.14159265359f - a[i],
                                3.14159265359f + a[i], 6.28318530718f - a[i]};
        for (int c = 0; c < 4; ++c) {
            float d = cands[c] - x;
            if (d <= 1.0e-6f) d += 6.28318530718f;
            if (d < best) best = d;
        }
    }
    return best;  /* delta angle to the next edge, (0, 2pi] */
}

void PatternModulator::scheduleNext(uint8_t phase) {
    if (phase > 2) return;

    const float omega = m_cmd_omega;
    if (omega < MIN_OMEGA) {
        return;  /* hold: too slow or reversing; retried on update */
    }

    const float offset = PHASE_OFFSET[phase];
    const float x = wrap2pi(m_cmd_theta + m_cmd_delta);
    const float dphi = nextEdgeAngle(x, offset);

    const float cnt_hz = TIM1_CLK_HZ / (static_cast<float>(TIM1->PSC) + 1.0f);
    const uint32_t arr = TIM1->ARR;
    const float counts = dphi / omega * cnt_hz;
    if (counts > static_cast<float>(arr + 1U)) {
        return;  /* more than a period out; retry on next update */
    }

    const uint32_t cnt = TIM1->CNT;
    uint32_t target;
    if (counts < 2.0f) {
        target = cnt + 2U;
    } else {
        target = cnt + static_cast<uint32_t>(counts);
    }
    target %= (arr + 1U);

    /* Level after the edge, from the pattern (never a toggle guess). */
    const float after = waveLevel(x + dphi + 1.0e-4f, offset);
    setOcMode(phase, (after > 0.0f) ? TIM_OCMODE_ACTIVE : TIM_OCMODE_INACTIVE);
    writeCcr(phase, target);
    m_level[phase] = after;
}

void PatternModulator::resync() {
    for (uint8_t ph = 0; ph < 3; ++ph) {
        scheduleNext(ph);
    }
}

bool PatternModulator::enable() {
    if (m_enabled) return false;
    if ((TIM1->CR1 & TIM_CR1_CEN) == 0U) return false;

    __disable_irq();

    /* Outputs off while the timer is reconfigured. */
    TIM1->BDTR &= ~TIM_BDTR_MOE;

    m_saved_arr = TIM1->ARR;

    /* Edge-aligned up counting at the same period: center-aligned period is
     * 2*(ARR+1) ticks, edge-aligned period is (ARR+1).  Doubling ARR keeps
     * the carrier (and the OC4 ADC trigger rate) unchanged. */
    TIM1->CR1 &= ~(TIM_CR1_CMS | TIM_CR1_DIR);
    TIM1->ARR = 2U * m_saved_arr + 1U;

    /* Initial levels from the pattern at the current angle (phase-locked
     * arm), then schedule the first edges. */
    for (uint8_t ph = 0; ph < 3; ++ph) {
        const float lvl = waveLevel(m_cmd_theta + m_cmd_delta, PHASE_OFFSET[ph]);
        setOcMode(ph, (lvl > 0.0f) ? TIM_OCMODE_ACTIVE : TIM_OCMODE_INACTIVE);
        m_level[ph] = lvl;
    }
    resync();

    /* Chained compare-match interrupts (priority above the update ISR). */
    TIM1->DIER |= TIM_DIER_CC1IE | TIM_DIER_CC2IE | TIM_DIER_CC3IE;
    HAL_NVIC_SetPriority(TIM1_CC_IRQn, 4, 0);
    HAL_NVIC_ClearPendingIRQ(TIM1_CC_IRQn);
    HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);

    m_enabled = true;
    m_moe_arm = true;  /* MOE set on the next update event, once the new ARR
                        * has taken effect (no ISR stall waiting for it). */
    __enable_irq();
    return true;
}

void PatternModulator::disable() {
    if (!m_enabled) return;

    __disable_irq();
    TIM1->BDTR &= ~TIM_BDTR_MOE;
    TIM1->DIER &= ~(TIM_DIER_CC1IE | TIM_DIER_CC2IE | TIM_DIER_CC3IE);
    HAL_NVIC_DisableIRQ(TIM1_CC_IRQn);

    /* Restore center-aligned PWM1 with 50% duties. */
    TIM1->ARR = m_saved_arr;
    TIM1->CR1 = (TIM1->CR1 & ~TIM_CR1_CMS) | TIM_COUNTERMODE_CENTERALIGNED1;
    const uint32_t mid = m_saved_arr / 2U;
    for (uint8_t ph = 0; ph < 3; ++ph) {
        setOcMode(ph, TIM_OCMODE_PWM1);
        writeCcr(ph, mid);
        m_level[ph] = 0.0f;
    }
    m_enabled = false;
    m_moe_arm = true;  /* 50% duty outputs restored on the next update. */
    __enable_irq();
}

void PatternModulator::onUpdateIsr() {
    /* Apply a pending MOE re-enable (set when the timer reconfig has had a
     * full period to settle). */
    if (m_moe_arm) {
        m_moe_arm = false;
        TIM1->BDTR |= TIM_BDTR_MOE;
    }
    if (!m_enabled) return;
    /* Refresh anything that failed to schedule (slow omega, long spans). */
    resync();
}

void PatternModulator::onMatchIsr(uint8_t phase) {
    if (!m_enabled || phase > 2) return;
    scheduleNext(phase);
}

} // namespace Inverter

/* TIME_DOMAIN: PWM_COMPARE_MATCH_ISR
 *   Fires on TIM1 compare matches while pattern mode is enabled.  Applies
 *   the pre-armed level for the edge and schedules the next one.
 * CODEGEN: Keep as-is; this is a base-image safety/scheduling hook.
 */
extern "C" void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef* htim) {
    if (htim->Instance != TIM1) return;
    uint8_t phase = 2;
    switch (htim->Channel) {
        case TIM_CHANNEL_1: phase = 0; break;
        case TIM_CHANNEL_2: phase = 1; break;
        case TIM_CHANNEL_3: phase = 2; break;
        default: return;
    }
    Inverter::patternModulator().onMatchIsr(phase);
}
