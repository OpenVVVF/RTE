#ifndef __INVERTER_PWM_H__
#define __INVERTER_PWM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define PWM_DEFAULT_SWITCHING_FREQ_HZ   10000U  /* 10 kHz */
#define PWM_DEFAULT_DEADTIME_NS         1000U   /* 1 us */

void PWM_SetFrequency(uint32_t freq_hz);
void PWM_SetDeadTime(uint32_t deadtime_ns);
void PWM_SetDutyCycle(uint8_t phase, float duty_percent);
void PWM_SetThreePhaseDuty(float duty_u, float duty_v, float duty_w);

/**
 * @brief Set a static three-phase voltage vector at the given electrical angle.
 *
 * The angle follows the same convention as the running SPWM ISR, with the
 * phase-U peak at +90 deg.  Calling this does NOT start the rotating SPWM ramp;
 * it simply parks the PWM outputs at the requested vector.
 */
void PWM_SetVoltageAngle(float angle_rad, float modulation_index);

/**
 * @brief Set a stationary-frame voltage vector using SVPWM.
 *
 * Converts Valpha/Vbeta to three-phase duty cycles and writes them directly.
 * Does NOT start any ISR ramp; intended for closed-loop control output.
 */
void PWM_SetVoltageVector(float valpha_v, float vbeta_v, float vdc_v);

/**
 * @brief Enable/disable the complementary (CHxN) outputs.
 *
 * When enabled, PWM_StartPhase also starts the low-side output with the
 * BDTR dead-time inserted between CHx and CHxN edges.
 */
void PWM_SetComplementary(bool enable);
bool PWM_GetComplementary(void);

/**
 * @brief Start the TIM1 update interrupt without starting the SPWM ramp.
 *
 * This is what drives the RTE tim_isr timing domain (10 kHz by default).
 */
void PWM_StartUpdateInterrupt(void);

/**
 * @brief Stop the TIM1 update interrupt only if SPWM is not running.
 */
void PWM_StopUpdateInterrupt(void);

float PWM_GetFrequency(void);
float PWM_GetUpdateFrequency(void);

void PWM_StartSPWM(float fundamental_freq_hz, float modulation_index);
void PWM_StopSPWM(void);
void PWM_SetSPWMParams(float fundamental_freq_hz, float modulation_index);

uint32_t PWM_GetSPWMElectricalCycles(void);
void PWM_ResetSPWMElectricalCycles(void);

/**
 * @brief Current electrical angle of the running SPWM ramp, in radians.
 *
 * Wraps every 2*pi.  Returns 0 if SPWM is not running.
 */
float PWM_GetSPWMAngle(void);

void PWM_StartPhase(uint8_t phase);
void PWM_StopPhase(uint8_t phase);
void PWM_Start(void);
void PWM_Stop(void);
void PWM_ClearFault(void);
void PWM_ClearBreakFlag(void);

#ifdef __cplusplus
}
#endif

#endif /* __INVERTER_PWM_H__ */
