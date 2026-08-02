#ifndef __INVERTER_PWM_H__
#define __INVERTER_PWM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define PWM_DEFAULT_SWITCHING_FREQ_HZ   2500U   /* 2.5 kHz PWM period, 5 kHz transistor switching */
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
 * it simply parks the PWM outputs at the requested vector.  Useful for rotor
 * alignment during calibration.
 */
void PWM_SetVoltageAngle(float angle_rad, float modulation_index);

/**
 * @brief Set a stationary-frame voltage vector using SVPWM.
 *
 * Converts Valpha/Vbeta to three-phase duty cycles and writes them directly.
 * Does NOT start any ISR ramp; intended for FOC output.
 */
void PWM_SetVoltageVector(float valpha_v, float vbeta_v, float vdc_v);

/**
 * @brief Enable FOC mode in the TIM1 update ISR.
 *
 * When active, the ISR will not run the open-loop SPWM ramp.  The actual FOC
 * computation is driven by FocControlManager; this flag only prevents SPWM
 * from overwriting the duty cycles.
 */
void PWM_EnableFocMode(void);
void PWM_DisableFocMode(void);
bool PWM_IsFocModeActive(void);

/**
 * @brief Start the TIM1 update interrupt without starting the SPWM ramp.
 *
 * Used by FOC to get a periodic ISR.  Safe to call while SPWM is running;
 * if SPWM is already running the interrupt is already enabled.
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

/**
 * @brief Force one phase leg's output level immediately (sync modulators).
 *
 * high=true forces OCxREF active (high-side on after dead-time insertion);
 * false forces inactive (low-side on).  The write is immediate (OCxM forced
 * mode has no preload); dead-time insertion and the BKIN break path stay in
 * effect, and TIM1 keeps running so the ADC trigger cadence is unchanged.
 * CH4 (the ADC trigger) is never touched.
 */
void PWM_ForcePhaseLevel(uint8_t phase, bool high);

/**
 * @brief Restore all three phase channels to PWM1 output-compare mode.
 *
 * The CCRs keep their last written values; preloaded duties apply from the
 * next update event, so an async modulator (SVPWM) resumes glitch-free.
 */
void PWM_ReleaseForcedOutputs(void);

void PWM_StartPhase(uint8_t phase);
void PWM_StopPhase(uint8_t phase);
void PWM_Start(void);
void PWM_Stop(void);
void PWM_ClearFault(void);
void PWM_ClearBreakFlag(void);

void PWM_PrintState(void);
void PWM_PrintSPWMState(void);

#ifdef __cplusplus
}
#endif

#endif /* __INVERTER_PWM_H__ */
