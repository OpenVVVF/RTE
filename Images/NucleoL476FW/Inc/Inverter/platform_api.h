#pragma once

/**
 * @brief Slim platform API exposed to RTE-generated code (Nucleo-L476RG).
 *
 * Each generated domain source file includes this header.  The functions below
 * are the only base-image services the generated code should call directly;
 * this keeps the base image / codegen contract small and stable.
 *
 * Signatures match the Gen6 platform_api.h subset so graphs built against the
 * Gen6 node templates keep working.  This board has no current/voltage/encoder
 * sensing wired up yet, so all sensor getters are stubs that return false / 0.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * PWM outputs
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
 * Sensor inputs (stubs - no sensing hardware on the bare Nucleo)
 * -------------------------------------------------------------------------- */

/**
 * @brief Read the latest PWM-synchronous phase currents [A].
 * @return false (no current sensing on this platform yet).
 */
bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a);

/**
 * @brief Read the latest encoder angle [deg, 0..360).
 * @return false (no encoder on this platform yet).
 */
bool platform_get_encoder_angle(float* angle_deg);

/**
 * @brief Non-destructive read of the latest encoder angle. Returns 0.
 */
float platform_get_encoder_angle_latest(void);

/**
 * @brief Latest DC-link voltage [V]. Returns 0.
 */
float platform_get_dc_link_voltage(void);

/**
 * @brief Throttle A input [0..1]. Returns 0.
 */
float platform_get_throttle_a(void);

/**
 * @brief Throttle B input [0..1]. Returns 0.
 */
float platform_get_throttle_b(void);

/**
 * @brief Motor temperature [deg C]. Returns 25.
 */
float platform_get_motor_temperature(void);

/**
 * @brief Inverter temperature for channel [deg C]. Returns 25.
 */
float platform_get_inverter_temperature(uint8_t channel);

/* --------------------------------------------------------------------------
 * Safety / faults
 * -------------------------------------------------------------------------- */

/**
 * @brief Raise a latched fault from generated code (stub: no fault manager).
 */
void platform_raise_fault(uint32_t source, uint8_t reason);

/**
 * @brief true if a Critical-severity fault is currently active. Returns false.
 */
bool platform_has_critical_fault(void);

/* --------------------------------------------------------------------------
 * Critical sections (for cross-domain bridges)
 * -------------------------------------------------------------------------- */

void platform_critical_enter(void);
void platform_critical_exit(void);

/* --------------------------------------------------------------------------
 * Config / persistence (RAM-backed only; no FRAM on this platform)
 * -------------------------------------------------------------------------- */

float platform_config_load(const char* key, float default_value);
void platform_config_set(const char* key, float value);
float platform_config_get(const char* key);

/* --------------------------------------------------------------------------
 * Telemetry (no transport wired up yet; calls are no-ops)
 * -------------------------------------------------------------------------- */

void platform_telemetry_log_f32(const char* key, float value);

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void);
uint32_t platform_micros(void);

#ifdef __cplusplus
}
#endif
