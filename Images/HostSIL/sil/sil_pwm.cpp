/*
 * sil_pwm.cpp — SIL replacement for Src/Inverter/Drivers/PWM/pwm.cpp.
 *
 * Reproduces the public pwm.h contract and, crucially, the TIM1 update-ISR
 * dispatch semantics of the hardware driver:
 *
 *   HAL_TIM_PeriodElapsedCallback (per update event)
 *     -> LoopStats::tim_isr++
 *     -> platform_set_current_domain_dt(1 / pwm_update_freq_hz)
 *     -> app::TimIsrStep(appState.tim_isr)   iff ControlSupervisor running
 *     -> FocControlManager_OnPwmPeriod()     iff FOC mode active
 *     -> open-loop SPWM ramp                 iff SPWM running
 *
 * Duties are latched into the fake TIM1 CCR registers exactly like the real
 * code (CCR = duty% * ARR / 100), so register-level readbacks
 * (PWM_GetCurrentDuties, FocControlManager telemetry) behave identically.
 *
 * The SIL scheduler fires update events at the firmware-visible update rate
 * (PWM_GetUpdateFrequency()); TRGO/injected conversions at the switching
 * rate.  See README for the rate-modeling note.
 */
#include "pwm.h"
#include "tim.h"
#include "Inverter/AppState.h"
#include "Inverter/LoopStats.h"
#include "Inverter/platform_api.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "mcp2221a_driver.h"

#include "domain_tim_isr_generated.h"

#include "sil_hooks.h"

#include <math.h>
#include <stdbool.h>

/* The real pwm.cpp declares a weak fallback; FocControlManager.cpp (compiled
 * verbatim) provides the strong definition. */
extern "C" void FocControlManager_OnPwmPeriod(void) __attribute__((weak));
extern "C" void FocControlManager_OnPwmPeriod(void) {}

#define TIM1_CLOCK_HZ       275000000UL
#define TIM_MAX_ARR         65535U
#define TWO_PI              6.283185307f

static const uint32_t pwm_phase_channels[3] = {
    TIM_CHANNEL_1,
    TIM_CHANNEL_2,
    TIM_CHANNEL_3
};

static volatile float pwm_switching_freq_hz = (float)PWM_DEFAULT_SWITCHING_FREQ_HZ;
static volatile float pwm_update_freq_hz    = (float)PWM_DEFAULT_SWITCHING_FREQ_HZ;

static volatile float    spwm_angle = 0.0f;
static volatile float    spwm_fundamental_freq_hz = 1.0f;
static volatile float    spwm_modulation_index = 0.0f;
static volatile uint8_t  spwm_running = 0;
static volatile uint32_t spwm_elec_cycles = 0;

static volatile uint8_t foc_active = 0;

static uint32_t PWM_PhaseToChannel(uint8_t phase)
{
    if (phase > 2) return 0;
    return pwm_phase_channels[phase];
}

void PWM_SetFrequency(uint32_t freq_hz)
{
    if (freq_hz == 0) return;

    uint32_t target = TIM1_CLOCK_HZ / (2UL * freq_hz);
    uint32_t psc = 0;
    uint32_t arr = target;

    while (arr > TIM_MAX_ARR)
    {
        psc++;
        arr = target / (psc + 1);
        if (psc > TIM_MAX_ARR) return;
    }

    __HAL_TIM_SET_PRESCALER(&htim1, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);

    pwm_switching_freq_hz = (float)TIM1_CLOCK_HZ /
                            (2.0f * (float)(arr + 1U) * (float)(psc + 1U));

    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;
}

void PWM_SetDeadTime(uint32_t deadtime_ns)
{
    /* Dead-time is a no-op in the averaged-duty SIL model; keep the BDTR DTG
     * bookkeeping so register dumps look sane. */
    uint32_t dtg = (deadtime_ns > 127U) ? 127U : deadtime_ns;
    uint32_t bdtr = TIM1->BDTR;
    bdtr &= ~TIM_BDTR_DTG;
    bdtr |= dtg;
    TIM1->BDTR = bdtr;
}

void PWM_SetDutyCycle(uint8_t phase, float duty_percent)
{
    if (phase > 2) return;
    if (duty_percent < 0.0f) duty_percent = 0.0f;
    if (duty_percent > 100.0f) duty_percent = 100.0f;

    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);
    uint32_t pulse = (uint32_t)((duty_percent * (float)arr) / 100.0f);

    uint32_t channel = PWM_PhaseToChannel(phase);
    __HAL_TIM_SET_COMPARE(&htim1, channel, pulse);
}

#define SVPWM_M_MAX 1.154700538f

void PWM_SetThreePhaseDuty(float duty_u, float duty_v, float duty_w)
{
    using Inverter::PhaseSwap;
    switch (Inverter::motorCalibration().phase_swap) {
        case PhaseSwap::SwapUV: {
            const float tmp = duty_u;
            duty_u = duty_v;
            duty_v = tmp;
            break;
        }
        case PhaseSwap::SwapVW: {
            const float tmp = duty_v;
            duty_v = duty_w;
            duty_w = tmp;
            break;
        }
        case PhaseSwap::SwapUW: {
            const float tmp = duty_u;
            duty_u = duty_w;
            duty_w = tmp;
            break;
        }
        default:
            break;
    }

    PWM_SetDutyCycle(0, duty_u);
    PWM_SetDutyCycle(1, duty_v);
    PWM_SetDutyCycle(2, duty_w);
}

void PWM_SetVoltageAngle(float angle_rad, float modulation_index)
{
    if (modulation_index < 0.0f) modulation_index = 0.0f;
    if (modulation_index > SVPWM_M_MAX) modulation_index = SVPWM_M_MAX;

    float u = modulation_index * sinf(angle_rad);
    float v = modulation_index * sinf(angle_rad - TWO_PI / 3.0f);
    float w = modulation_index * sinf(angle_rad + TWO_PI / 3.0f);

    float v_max = (u > v) ? ((u > w) ? u : w) : ((v > w) ? v : w);
    float v_min = (u < v) ? ((u < w) ? u : w) : ((v < w) ? v : w);
    float v0 = -0.5f * (v_max + v_min);

    float du = 50.0f + 50.0f * (u + v0);
    float dv = 50.0f + 50.0f * (v + v0);
    float dw = 50.0f + 50.0f * (w + v0);

    if (du < 0.0f) du = 0.0f; else if (du > 100.0f) du = 100.0f;
    if (dv < 0.0f) dv = 0.0f; else if (dv > 100.0f) dv = 100.0f;
    if (dw < 0.0f) dw = 0.0f; else if (dw > 100.0f) dw = 100.0f;

    PWM_SetThreePhaseDuty(du, dv, dw);
}

void PWM_SetVoltageVector(float valpha_v, float vbeta_v, float vdc_v)
{
    if (vdc_v <= 1.0f) {
        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        return;
    }

    const float sqrt3 = 1.7320508075688772f;
    float valpha = valpha_v;
    float vbeta  = vbeta_v;
    const float v_max_linear = (vdc_v / sqrt3) * 0.95f;
    const float v_albe_sq = valpha * valpha + vbeta * vbeta;
    if (v_albe_sq > v_max_linear * v_max_linear && v_albe_sq > 1e-12f) {
        const float scale = v_max_linear / sqrtf(v_albe_sq);
        valpha *= scale;
        vbeta  *= scale;
    }

    float va = valpha;
    float vb = -0.5f * valpha + 0.5f * sqrt3 * vbeta;
    float vc = -0.5f * valpha - 0.5f * sqrt3 * vbeta;

    float v_max = (va > vb) ? ((va > vc) ? va : vc) : ((vb > vc) ? vb : vc);
    float v_min = (va < vb) ? ((va < vc) ? va : vc) : ((vb < vc) ? vb : vc);
    float vcom = 0.5f * (v_max + v_min);

    float du = 50.0f + 50.0f * (va - vcom) / vdc_v;
    float dv = 50.0f + 50.0f * (vb - vcom) / vdc_v;
    float dw = 50.0f + 50.0f * (vc - vcom) / vdc_v;

    if (du < 0.0f) du = 0.0f; else if (du > 100.0f) du = 100.0f;
    if (dv < 0.0f) dv = 0.0f; else if (dv > 100.0f) dv = 100.0f;
    if (dw < 0.0f) dw = 0.0f; else if (dw > 100.0f) dw = 100.0f;

    PWM_SetThreePhaseDuty(du, dv, dw);
}

void PWM_EnableFocMode(void)
{
    foc_active = 1;
    TIM1->RCR = 0U;
    pwm_update_freq_hz = 2.0f * pwm_switching_freq_hz;
}

void PWM_DisableFocMode(void)
{
    foc_active = 0;
    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;
}

bool PWM_IsFocModeActive(void)
{
    return foc_active != 0;
}

void PWM_StartUpdateInterrupt(void)
{
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);
}

void PWM_StopUpdateInterrupt(void)
{
    if (!spwm_running) {
        __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
    }
}

float PWM_GetFrequency(void)
{
    return pwm_switching_freq_hz;
}

float PWM_GetUpdateFrequency(void)
{
    return pwm_update_freq_hz;
}

void PWM_GetCurrentDuties(float* duty_u, float* duty_v, float* duty_w)
{
    const uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);
    if (arr == 0U) {
        *duty_u = 0.0f;
        *duty_v = 0.0f;
        *duty_w = 0.0f;
        return;
    }
    *duty_u = 100.0f * (float)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_1) / (float)arr;
    *duty_v = 100.0f * (float)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_2) / (float)arr;
    *duty_w = 100.0f * (float)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_3) / (float)arr;
}

bool PWM_FindSafeSamplePoint(float duty_u, float duty_v, float duty_w,
                             uint32_t arr, uint32_t min_gap_ticks,
                             uint32_t* out_ccr4, uint32_t* out_gap_ticks)
{
    if (duty_u < 0.0f) duty_u = 0.0f; else if (duty_u > 100.0f) duty_u = 100.0f;
    if (duty_v < 0.0f) duty_v = 0.0f; else if (duty_v > 100.0f) duty_v = 100.0f;
    if (duty_w < 0.0f) duty_w = 0.0f; else if (duty_w > 100.0f) duty_w = 100.0f;

    const uint32_t ccr_u = (uint32_t)((duty_u * (float)arr) / 100.0f);
    const uint32_t ccr_v = (uint32_t)((duty_v * (float)arr) / 100.0f);
    const uint32_t ccr_w = (uint32_t)((duty_w * (float)arr) / 100.0f);

    uint32_t min_ccr = ccr_u;
    if (ccr_v < min_ccr) min_ccr = ccr_v;
    if (ccr_w < min_ccr) min_ccr = ccr_w;

    uint32_t max_ccr = ccr_u;
    if (ccr_v > max_ccr) max_ccr = ccr_v;
    if (ccr_w > max_ccr) max_ccr = ccr_w;

    const uint32_t gap_all_high = 2U * min_ccr;
    const uint32_t gap_all_low  = 2U * (arr - max_ccr);

    uint32_t best_gap = 0;
    uint32_t best_mid = 0;
    if (gap_all_low >= gap_all_high) {
        best_gap = gap_all_low;
        best_mid = (max_ccr + arr) / 2U;
    } else {
        best_gap = gap_all_high;
        best_mid = min_ccr / 2U;
    }

    if (best_gap < min_gap_ticks) {
        *out_gap_ticks = best_gap;
        return false;
    }

    *out_ccr4 = best_mid;
    *out_gap_ticks = best_gap;
    return true;
}

void PWM_StartSPWM(float fundamental_freq_hz, float modulation_index)
{
    if (modulation_index < 0.0f) modulation_index = 0.0f;
    if (modulation_index > SVPWM_M_MAX) modulation_index = SVPWM_M_MAX;

    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;

    spwm_fundamental_freq_hz = fundamental_freq_hz;
    spwm_modulation_index = modulation_index;
    spwm_angle = 0.0f;
    spwm_running = 1;

    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);
}

void PWM_StopSPWM(void)
{
    spwm_running = 0;
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
}

void PWM_SetSPWMParams(float fundamental_freq_hz, float modulation_index)
{
    if (modulation_index < 0.0f) modulation_index = 0.0f;
    if (modulation_index > SVPWM_M_MAX) modulation_index = SVPWM_M_MAX;

    spwm_fundamental_freq_hz = fundamental_freq_hz;
    spwm_modulation_index = modulation_index;
}

/* TIM1 update ISR callback.  The emitter fills the // RTE_EMIT marker in the
 * hardware tree with app::TimIsrStep(appState.tim_isr); this SIL copy carries
 * that expansion by hand (same gating, same order). */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM1) return;
    ++Inverter::LoopStats::tim_isr;

    platform_set_current_domain_dt(1.0f / pwm_update_freq_hz);

    if (Inverter::ControlSupervisor::instance().isRunning()) {
        app::TimIsrStep(appState.tim_isr);
    }

    if (foc_active) {
        FocControlManager_OnPwmPeriod();
        return;
    }

    if (!spwm_running) return;

    float angle = spwm_angle;
    float m = spwm_modulation_index;

    float u = m * sinf(angle);
    float v = m * sinf(angle - TWO_PI / 3.0f);
    float w = m * sinf(angle + TWO_PI / 3.0f);

    float v_max = (u > v) ? ((u > w) ? u : w) : ((v > w) ? v : w);
    float v_min = (u < v) ? ((u < w) ? u : w) : ((v < w) ? v : w);
    float v0 = -0.5f * (v_max + v_min);

    float du = 50.0f + 50.0f * (u + v0);
    float dv = 50.0f + 50.0f * (v + v0);
    float dw = 50.0f + 50.0f * (w + v0);

    if (du < 0.0f) du = 0.0f; else if (du > 100.0f) du = 100.0f;
    if (dv < 0.0f) dv = 0.0f; else if (dv > 100.0f) dv = 100.0f;
    if (dw < 0.0f) dw = 0.0f; else if (dw > 100.0f) dw = 100.0f;

    PWM_SetThreePhaseDuty(du, dv, dw);

    angle += TWO_PI * spwm_fundamental_freq_hz / pwm_switching_freq_hz;
    if (angle >= TWO_PI) {
        angle -= TWO_PI;
        ++spwm_elec_cycles;
    } else if (angle < 0.0f) {
        angle += TWO_PI;
    }
    spwm_angle = angle;
}

uint32_t PWM_GetSPWMElectricalCycles(void)
{
    return spwm_elec_cycles;
}

void PWM_ResetSPWMElectricalCycles(void)
{
    spwm_elec_cycles = 0;
}

float PWM_GetSPWMAngle(void)
{
    return spwm_running ? spwm_angle : 0.0f;
}

void PWM_StartPhase(uint8_t phase)
{
    if (phase > 2) return;
    uint32_t channel = PWM_PhaseToChannel(phase);
    HAL_TIM_PWM_Start(&htim1, channel);
    HAL_TIMEx_PWMN_Start(&htim1, channel);
}

void PWM_StopPhase(uint8_t phase)
{
    if (phase > 2) return;
    uint32_t channel = PWM_PhaseToChannel(phase);
    HAL_TIM_PWM_Stop(&htim1, channel);
    HAL_TIMEx_PWMN_Stop(&htim1, channel);
}

void PWM_Start(void)
{
    PWM_StartPhase(0);
    PWM_StartPhase(1);
    PWM_StartPhase(2);
}

void PWM_Stop(void)
{
    PWM_StopPhase(0);
    PWM_StopPhase(1);
    PWM_StopPhase(2);
}

void PWM_ClearFault(void)
{
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_BREAK);
    __HAL_TIM_MOE_ENABLE(&htim1);
}

void PWM_ClearBreakFlag(void)
{
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_BREAK);
}

void PWM_PrintState(void)
{
    uint32_t bdtr = TIM1->BDTR;

    MCP2221A_Printf("[PWM] BDTR=0x%04lX | MOE=%lu | DTG=0x%02lX\r\n",
                     (unsigned long)bdtr, (unsigned long)((bdtr >> 15) & 1),
                     (unsigned long)(bdtr & TIM_BDTR_DTG));
}

void PWM_PrintSPWMState(void)
{
    float du = 0.0f, dv = 0.0f, dw = 0.0f;
    PWM_GetCurrentDuties(&du, &dv, &dw);

    MCP2221A_Printf("[SPWM] running=%u f=%.2f Hz m=%.3f | duties U=%.1f V=%.1f W=%.1f %%\r\n",
                     (unsigned)spwm_running,
                     (double)spwm_fundamental_freq_hz,
                     (double)spwm_modulation_index,
                     (double)du, (double)dv, (double)dw);
}

/* --------------------------------------------------------------------------
 * SIL scheduler hooks
 * ------------------------------------------------------------------------ */

float silTimSwitchingHz() {
    return pwm_switching_freq_hz;
}

float silTimUpdateHz() {
    return pwm_update_freq_hz;
}

bool silTimBaseRunning() {
    return htim1.sil_base_running != 0;
}

bool silTimUpdateIrqEnabled() {
    return (sil_tim1.DIER & TIM_IT_UPDATE) != 0U;
}

bool silTimOutputsDriving() {
    const bool moe = (sil_tim1.BDTR & TIM_BDTR_MOE) != 0U;
    const bool ch123 = (htim1.sil_active_channels & 0x7U) == 0x7U;
    return moe && ch123 && silGateOutputsEnabled();
}

void silTimFireUpdateIrq() {
    HAL_TIM_PeriodElapsedCallback(&htim1);
}
