#pragma once

/**
 * @brief Platform API exposed to RTE-generated code.
 *
 * Each generated domain source file includes this header.  The functions below
 * are the only base-image services the generated code should call directly;
 * this keeps the base image / codegen contract small and stable.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * PWM / gate-driver outputs
 * -------------------------------------------------------------------------- */

/**
 * @brief Set raw three-phase PWM duties [0, 100].
 */
void platform_pwm_set(float du, float dv, float dw);

/**
 * @brief Set a voltage vector using the base-image SVPWM implementation.
 * @param valpha  Alpha-axis voltage [V].
 * @param vbeta   Beta-axis voltage [V].
 * @param vdc     DC-link voltage [V].
 */
void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc);

/* --------------------------------------------------------------------------
 * Sensor inputs
 * -------------------------------------------------------------------------- */

/**
 * @brief Read the latest PWM-synchronous phase currents [A].
 * @return true if a new sample was available.
 */
bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a);

/**
 * @brief Read the latest encoder angle [deg, 0..360).
 * @return true if a new sample was available.
 */
bool platform_get_encoder_angle(float* angle_deg);

/**
 * @brief Latest DC-link voltage [V].
 */
float platform_get_dc_link_voltage(void);

/**
 * @brief Throttle A input [0..1] (codegen application layer fills the sampler).
 */
float platform_get_throttle_a(void);

/**
 * @brief Throttle B input [0..1] (codegen application layer fills the sampler).
 */
float platform_get_throttle_b(void);

/**
 * @brief Motor temperature [degC] (codegen application layer fills the sampler).
 */
float platform_get_motor_temperature(void);

/**
 * @brief Inverter temperature [degC] for one of the NTC channels.
 * @param channel 0..2 maps to AIN_TMP_SENSE_1..3.
 */
float platform_get_inverter_temperature(uint8_t channel);

/**
 * @brief Trigger a one-shot sample of all slow application analog inputs.
 *
 * This is a codegen insertion point: the base image provides the sampler, and
 * the app_loop domain may call it once per iteration.  Until the sampler is
 * implemented the getters above return 0.
 */
void platform_sample_application_sensors(void);

/* --------------------------------------------------------------------------
 * Safety / faults
 * -------------------------------------------------------------------------- */

/**
 * @brief Raise a latched fault from generated code.
 *
 * Prefer the typed FaultManager interface in C++ code; this C API exists for
 * generated code that may be compiled as C or C++.
 */
void platform_raise_fault(uint32_t source, uint8_t reason);

/**
 * @brief true if a Critical-severity fault is currently active.
 */
bool platform_has_critical_fault(void);

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void);
uint32_t platform_micros(void);

#ifdef __cplusplus
}
#endif
