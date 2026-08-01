/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    pwm.c
  * @brief   Three-phase PWM / SPWM control for TIM1 gate-driver output.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "pwm.h"
#include "tim.h"
#include "Inverter/AppState.h"
#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/LoopStats.h"
#include "mcp2221a_driver.h"
#include <math.h>
#include <stdbool.h>

/* Forward declaration for the FOC ISR hook (defined in FocControlManager.cpp).
 * Stubbed out when FOC is removed from the base image. */
extern "C" void FocControlManager_OnPwmPeriod(void) __attribute__((weak));
extern "C" void FocControlManager_OnPwmPeriod(void) {}

/* USER CODE BEGIN 0 */
#define TIM1_CLOCK_HZ       275000000UL
#define TIM_MAX_ARR         65535U
#define TWO_PI              6.283185307f

/* Phase-to-timer-channel mapping.
 * Index 0 = phase U, 1 = phase V, 2 = phase W.
 */
static const uint32_t pwm_phase_channels[3] = {
    TIM_CHANNEL_1,
    TIM_CHANNEL_2,
    TIM_CHANNEL_3
};

/* Current switching frequency, updated by PWM_SetFrequency. */
static volatile float pwm_switching_freq_hz = (float)PWM_DEFAULT_SWITCHING_FREQ_HZ;

/* Current TIM1 update frequency.  For center-aligned PWM with RCR=1 this equals
 * the switching frequency; with RCR=0 it is twice the switching frequency
 * (dual-update FOC). */
static volatile float pwm_update_freq_hz = (float)PWM_DEFAULT_SWITCHING_FREQ_HZ;

/* Closed-loop control hook.  When set, the TIM1 update ISR runs this hook
 * (the controller, which drives the SVPWM modulator via PWM_SetVoltageVector)
 * instead of the active self-clocked modulator (SPWM).  Registered by
 * PWM_EnableFocMode(); nullptr in open-loop / idle. */
static void (*pwm_control_hook)(void) = nullptr;

/* CKD = DIV1  =>  t_DTS = 1 / TIM1_CLOCK_HZ.
 * Map the requested deadtime to the finest STM32 DTG encoding. */
static uint8_t PWM_ComputeDeadTime(uint32_t deadtime_ns)
{
    const uint32_t ticks = (uint32_t)((deadtime_ns * (uint64_t)TIM1_CLOCK_HZ +
                                       500000000ULL) / 1000000000ULL);

    if (ticks <= 127U) {
        /* 0xx: DT = DTG[7:0] * t_DTS */
        return (uint8_t)ticks;
    } else if (ticks <= 254U) {
        /* 10x: DT = (64 + DTG[5:0]) * 2 * t_DTS */
        uint32_t dtg = (ticks / 2U) - 64U;
        if (dtg > 63U) dtg = 63U;
        return (uint8_t)(0x80U | dtg);
    } else if (ticks <= 736U) {
        /* 110: DT = (32 + DTG[4:0] * 2) * 8 * t_DTS */
        uint32_t dtg = (ticks / 8U - 32U + 1U) / 2U;
        if (dtg > 31U) dtg = 31U;
        return (uint8_t)(0xC0U | dtg);
    } else {
        /* 111: DT = (32 + DTG[4:0]) * 16 * t_DTS */
        uint32_t dtg = (ticks / 16U) - 32U;
        if (dtg > 31U) dtg = 31U;
        return (uint8_t)(0xE0U | dtg);
    }
}

static uint32_t PWM_PhaseToChannel(uint8_t phase)
{
    if (phase > 2) return 0;
    return pwm_phase_channels[phase];
}

void PWM_SetFrequency(uint32_t freq_hz)
{
    if (freq_hz == 0) return;

    /* Center-aligned: f = f_tim / ((PSC+1) * 2 * ARR)
       ARR is 16-bit (max 65535), so we may need to raise PSC. */
    uint32_t target = TIM1_CLOCK_HZ / (2UL * freq_hz);
    uint32_t psc = 0;
    uint32_t arr = target;

    while (arr > TIM_MAX_ARR)
    {
        psc++;
        arr = target / (psc + 1);
        if (psc > TIM_MAX_ARR) return; /* cannot achieve */
    }

    __HAL_TIM_SET_PRESCALER(&htim1, psc);
    __HAL_TIM_SET_AUTORELOAD(&htim1, arr);

    pwm_switching_freq_hz = (float)TIM1_CLOCK_HZ /
                            (2.0f * (float)(arr + 1U) * (float)(psc + 1U));

    /* Default to one update event per switching period (RCR=1), matching the
     * open-loop SPWM convention.  FOC mode will override this when enabled. */
    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;
}

void PWM_SetDeadTime(uint32_t deadtime_ns)
{
    /* Update DTG field in BDTR while preserving break/dead-time configuration.
       MOE should be disabled before changing deadtime if PWM is running. */
    uint32_t dtg = (uint32_t)PWM_ComputeDeadTime(deadtime_ns);
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

/* SVPWM linear over-modulation limit: 2/sqrt(3) */
#define SVPWM_M_MAX 1.154700538f

void PWM_SetThreePhaseDuty(float duty_u, float duty_v, float duty_w)
{
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

    /* Min-max SVPWM zero-sequence injection. */
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
    /* SVPWM modulator (externally clocked): compute duties, then write them.
     * CCR preload makes the write land atomically at the next update event. */
    Inverter::svpwmModulator().update(valpha_v, vbeta_v, vdc_v);
    Inverter::svpwmModulator().commit();
}

/* TIME_DOMAIN: CLOSED_LOOP_MODULATION_START
 *   Switches TIM1 update ISR to the closed-loop control path (FOC, etc.).
 * CODEGEN: Generalize to any closed-loop control/modulation combination.
 */
void PWM_EnableFocMode(void)
{
    /* FOC drives the SVPWM modulator from the control hook. */
    pwm_control_hook = FocControlManager_OnPwmPeriod;
    Inverter::setActiveModulator(&Inverter::svpwmModulator());
    /* Dual-update FOC: run the control ISR at twice the PWM switching frequency
     * (both top and bottom of the center-aligned triangle).  RCR=0 generates an
     * update event on every counter overflow/underflow. */
    TIM1->RCR = 0U;
    pwm_update_freq_hz = 2.0f * pwm_switching_freq_hz;
}

void PWM_DisableFocMode(void)
{
    pwm_control_hook = nullptr;
    if (Inverter::activeModulator() == &Inverter::svpwmModulator()) {
        Inverter::setActiveModulator(nullptr);
    }
    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;
}

bool PWM_IsFocModeActive(void)
{
    return pwm_control_hook != nullptr;
}

void PWM_StartUpdateInterrupt(void)
{
    HAL_NVIC_SetPriority(TIM1_UP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_IRQn);
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);
}

void PWM_StopUpdateInterrupt(void)
{
    if (!Inverter::spwmIsRunning()) {
        __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
        HAL_NVIC_DisableIRQ(TIM1_UP_IRQn);
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

/* TIME_DOMAIN: OPEN_LOOP_MODULATION_START
 *   Enables the SPWM angle ramp inside HAL_TIM_PeriodElapsedCallback.
 * CODEGEN: This is one example modulation.  Codegen may generate equivalent
 *   start/stop functions for SHEPWM, DPWM, six-step, etc.
 */
void PWM_StartSPWM(float fundamental_freq_hz, float modulation_index)
{
    /* SPWM uses one update event per switching period (RCR=1) so the angle
     * advances at the switching frequency. */
    TIM1->RCR = 1U;
    pwm_update_freq_hz = pwm_switching_freq_hz;

    /* Hand the modulation slot to the self-clocked SPWM modulator. */
    Inverter::spwmSetParams(fundamental_freq_hz, modulation_index);
    Inverter::spwmModulator().enter(0.0f, 0.0f);
    Inverter::setActiveModulator(&Inverter::spwmModulator());

    HAL_NVIC_SetPriority(TIM1_UP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_IRQn);
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);
}

void PWM_StopSPWM(void)
{
    Inverter::spwmModulator().exit();
    if (Inverter::activeModulator() == &Inverter::spwmModulator()) {
        Inverter::setActiveModulator(nullptr);
    }
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
    HAL_NVIC_DisableIRQ(TIM1_UP_IRQn);
}

void PWM_SetSPWMParams(float fundamental_freq_hz, float modulation_index)
{
    Inverter::spwmSetParams(fundamental_freq_hz, modulation_index);
}

/* TIM1 update ISR callback. Runs at the PWM switching frequency. */
/* TIME_DOMAIN: TIM1_PWM_UPDATE_ISR / MODULATION_TIME_DOMAIN
 *   Rate: PWM update frequency (see pwm_update_freq_hz).  Runs in ISR context.
 *   This is the dispatch point for all modulation strategies.
 * CODEGEN: Replace/extend the body of this callback with codegen-selected
 *   modulation: SPWM, SVPWM, SHEPWM, DPWM, etc.  Codegen also wires the
 *   selected control loop (open-loop, FOC, MPC, etc.) to this interrupt.
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM1) return;
    ++Inverter::LoopStats::tim_isr;

    /* RTE codegen: PWM-synchronous control + modulation step.
     * Generated code reads sensors, runs the selected control law, and writes
     * PWM duties.  The example FOC/SPWM code below is reference only; it is
     * superseded once the graph contains nodes assigned to the tim_isr domain. */
    // RTE_EMIT: tim_isr step

    if (pwm_control_hook != nullptr) {
        pwm_control_hook();
        return;
    }

    /* Drive the active self-clocked modulator (SPWM today).  Externally
     * clocked modulators (SVPWM) are driven by their control-loop caller;
     * future synchronous modulators (SHEPWM) run on their own timebase. */
    Inverter::Modulator* mod = Inverter::activeModulator();
    if (mod == nullptr || !mod->runsInPwmIsr()) return;

    mod->update(0.0f, 0.0f, 0.0f);
    mod->commit();
}

uint32_t PWM_GetSPWMElectricalCycles(void)
{
    return Inverter::spwmElectricalCycles();
}

void PWM_ResetSPWMElectricalCycles(void)
{
    Inverter::spwmResetElectricalCycles();
}

float PWM_GetSPWMAngle(void)
{
    return Inverter::spwmAngleRad();
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
    uint32_t sr   = TIM1->SR;
    uint32_t ccer = TIM1->CCER;

    MCP2221A_Printf("[PWM] BDTR=0x%04lX | MOE=%lu | BKP=%lu | DTG=0x%02lX\r\n",
                     bdtr, (bdtr >> 15) & 1, (bdtr >> 13) & 1, bdtr & TIM_BDTR_DTG);
    MCP2221A_Printf("[PWM] SR=0x%04lX | BIF=%lu | BKF=%lu\r\n",
                     sr, (sr >> 7) & 1, (sr >> 6) & 1);
    MCP2221A_Printf("[PWM] CCER=0x%04lX | CH1E=%lu CH1NE=%lu | CH2E=%lu CH2NE=%lu | CH3E=%lu CH3NE=%lu\r\n",
                     ccer,
                     (ccer >> 0) & 1, (ccer >> 2) & 1,
                     (ccer >> 4) & 1, (ccer >> 6) & 1,
                     (ccer >> 8) & 1, (ccer >> 10) & 1);
}

void PWM_PrintSPWMState(void)
{
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);
    float du = (arr == 0) ? 0.0f : (__HAL_TIM_GET_COMPARE(&htim1, pwm_phase_channels[0]) * 100.0f / (float)arr);
    float dv = (arr == 0) ? 0.0f : (__HAL_TIM_GET_COMPARE(&htim1, pwm_phase_channels[1]) * 100.0f / (float)arr);
    float dw = (arr == 0) ? 0.0f : (__HAL_TIM_GET_COMPARE(&htim1, pwm_phase_channels[2]) * 100.0f / (float)arr);

    MCP2221A_Printf("[SPWM] running=%u f=%.2f Hz m=%.3f | duties U=%.1f V=%.1f W=%.1f %%\r\n",
                     (unsigned)(Inverter::spwmIsRunning() ? 1U : 0U),
                     (double)Inverter::spwmFundamentalFreqHz(),
                     (double)Inverter::spwmModulationIndex(),
                     (double)du, (double)dv, (double)dw);
}
