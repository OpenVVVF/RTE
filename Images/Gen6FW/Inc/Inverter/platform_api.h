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

/* --------------------------------------------------------------------------
 * ADC injected conversion results (valid only in ADC ISR context)
 * -------------------------------------------------------------------------- */

uint32_t platform_adc_get_injected_u_sig(void);
uint32_t platform_adc_get_injected_v_sig(void);
uint32_t platform_adc_get_injected_u_ref(void);
uint32_t platform_adc_get_injected_v_ref(void);

/**
 * @brief Calibrated zero-current offsets [A] from the base image startup calibration.
 *
 * These are the offsets subtracted by the base image's PhaseCurrentADC; generated
 * code that reads raw injected ADC values should subtract the same offsets to match
 * the base image's current convention.
 */
float platform_adc_get_offset_u_a(void);
float platform_adc_get_offset_v_a(void);

/**
 * @brief Read the latest encoder angle [deg, 0..360).
 * @return true if a new sample was available.
 */
bool platform_get_encoder_angle(float* angle_deg);

/**
 * @brief Non-destructive read of the latest encoder angle [deg, 0..360).
 *
 * Unlike platform_get_encoder_angle(), this does not clear the new-data flag,
 * so multiple domains can read the same sample.  Returns the last computed
 * angle (0 if none yet).
 */
float platform_get_encoder_angle_latest(void);

/**
 * @brief Latest DC-link voltage [V].
 */
float platform_get_dc_link_voltage(void);

/* Phase voltages from the MAX22530 isolated ADC (filtered reads).
 * Channel map: 0=U, 1=V, 2=W, 3=DC link. */
float platform_phase_voltage_u(void);
float platform_phase_voltage_v(void);
float platform_phase_voltage_w(void);

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
 * Critical sections (for cross-domain bridges)
 * -------------------------------------------------------------------------- */

void platform_critical_enter(void);
void platform_critical_exit(void);

/* --------------------------------------------------------------------------
 * Config / persistence (for generated config nodes)
 * -------------------------------------------------------------------------- */

/**
 * @brief Register a config key and load its persisted value from FRAM.
 *
 * If the key exists in FRAM, the stored value is returned.  Otherwise the
 * default is returned and the key is created with the default value.
 * The key is also registered in the base-image command registry so the
 * 'config' shell command can get/set it at runtime.
 *
 * @param key     Config key string (max 31 chars).
 * @param default_value  Default value if key not found in FRAM.
 * @return Current value (persisted or default).
 */
float platform_config_load(const char* key, float default_value);

/**
 * @brief Set a config value and persist it to FRAM.
 */
void platform_config_set(const char* key, float value);

/**
 * @brief Get a config value.  Returns 0.0f if not found.
 */
float platform_config_get(const char* key);

/* --------------------------------------------------------------------------
 * Telemetry
 * -------------------------------------------------------------------------- */

/**
 * @brief Log a float value to the telemetry stream.
 * @param key   Telemetry key (max 31 chars, NUL-terminated).
 * @param value Value to send.
 */
void platform_telemetry_log_f32(const char* key, float value);

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void);
uint32_t platform_micros(void);

#ifdef __cplusplus
}
#endif
