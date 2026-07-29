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

/**
 * @brief Latest DC-link current [A] and input power [W].
 * Zero until the startup zero-offset capture completes (~2 s).
 */
float platform_get_dc_link_current(void);
float platform_get_dc_link_power(void);

/* Phase voltages from the MAX22530 isolated ADC (filtered reads).
 * Channel map: 0=U, 1=V, 2=W, 3=DC link. */
float platform_phase_voltage_u(void);
float platform_phase_voltage_v(void);
float platform_phase_voltage_w(void);

/**
 * @brief Throttle A input, normalized [0..1] (0 while implausible).
 */
float platform_get_throttle_a(void);

/**
 * @brief Throttle B input, normalized [0..1] (0 while implausible).
 */
float platform_get_throttle_b(void);

/**
 * @brief true while throttle A/B agree within the plausibility tolerance.
 */
bool platform_get_throttle_valid(void);

/**
 * @brief Motor temperature [degC] (codegen application layer fills the sampler).
 */
float platform_get_motor_temperature(void);

/**
 * @brief Inverter temperature [degC] for one of the NTC channels.
 * @param channel 0..2 maps to AIN_TMP_SENSE_1..3.
 */
float platform_get_inverter_temperature(uint8_t channel);

/* --------------------------------------------------------------------------
 * User digital IO
 *
 * platform_digital_read:  pin 1..8 -> USER_DIN_1..8; invalid pin returns false.
 * platform_digital_write: pin 1..4 -> USER_DOUT_1..4, 5 -> green LED,
 *                         6 -> orange LED; invalid pin is ignored.
 * -------------------------------------------------------------------------- */
bool platform_digital_read(uint8_t pin);
void platform_digital_write(uint8_t pin, bool value);

/* --------------------------------------------------------------------------
 * CAN bus (CanBus driver; buses: 0 = FDCAN1 "A", 1 = FDCAN2 "B")
 * -------------------------------------------------------------------------- */

/** @brief Queue a classic CAN frame for transmission (never blocks).
 *  @param bus  1 = "A"/FDCAN1, 2 = "B"/FDCAN2. */
bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                       const uint8_t* data, uint8_t dlc);

/**
 * @brief Latest received frame with an exact ID match.
 * Writes up to 8 payload bytes into data (may be stale; use seq_out to
 * detect new arrivals) and the mailbox sequence counter into seq_out.
 * @param bus  1 = "A"/FDCAN1, 2 = "B"/FDCAN2.
 * @return payload length 0..8, or -1 if no frame with this ID yet.
 */
int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out);

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
