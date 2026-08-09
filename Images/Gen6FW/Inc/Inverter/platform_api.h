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
 * @brief Enable/disable observer-based phase-current feedback for generated
 *        FOC graphs.
 *
 * When enabled, the `hw.adc.phase_currents` node template outputs the
 * CurrentObserver's predicted/corrected currents instead of raw ADC samples.
 * The flag is settable at runtime via the `obs` shell command.
 */
void platform_set_use_observer(bool enabled);
bool platform_get_use_observer(void);

/**
 * @brief Read the observer-estimated phase currents [A].
 *
 * Only valid when the observer has been corrected at least once.
 */
void platform_get_observer_currents(float* iu_a, float* iv_a, float* iw_a);

/**
 * @brief Run the current observer prediction step.
 *
 * Called by the generated TIM ISR domain before the control loop consumes
 * observer currents.
 */
void platform_observer_predict(float valpha_v, float vbeta_v,
                               float theta_elec_rad, float dt_s);

/**
 * @brief Set motor parameters for the current observer.
 *
 * Called by generated init code or a config node to seed the observer with
 * the correct motor R/L/flux values.
 */
void platform_observer_set_motor_params(float r_ohm, float l_henry,
                                        float flux_linkage_wb,
                                        float pole_pairs);

/**
 * @brief Initialize the current observer from the latest motor calibration.
 *
 * Convenience wrapper for generated init code that does not have direct
 * access to the MotorCalibration struct.
 */
void platform_observer_init_from_calibration(void);

/**
 * @brief Read the latest micro-burst sample from the phase-current ADC.
 *
 * Returns two points per phase (rank1/2 and rank3/4 of the injected
 * sequence).  Valid only in ADC ISR context.
 */
bool platform_adc_get_burst_sample(float* iu0_a, float* iv0_a,
                                   float* iu1_a, float* iv1_a,
                                   uint32_t* time_us);

/**
 * @brief Manually correct the current observer with a measured burst.
 *
 * Called by the generated ADC ISR domain after reading the burst sample.
 */
void platform_observer_correct(float iu_meas_a, float iv_meas_a,
                               float diudt_a_per_s, float divdt_a_per_s,
                               uint32_t t_us);

/**
 * @brief Schedule the next ADC trigger at the quietest point in the upcoming
 *        PWM period.
 *
 * Called by the generated TIM ISR domain after the new duties have been
 * computed.  Falls back to the legacy bottom trigger when no clean window
 * exists.
 *
 * @param duty_u  Phase U duty [0, 100].
 * @param duty_v  Phase V duty [0, 100].
 * @param duty_w  Phase W duty [0, 100].
 * @param arr     TIM1 auto-reload value.
 * @return Largest gap found, in timer ticks (0 if fallback used).
 */
uint32_t platform_schedule_adaptive_sample(float duty_u, float duty_v,
                                           float duty_w, uint32_t arr);

/**
 * @brief Current TIM1 auto-reload value (for generated code).
 */
uint32_t platform_pwm_get_arr(void);

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
 * @brief Mechanical speed estimate [rpm], signed by direction.
 *
 * EMA-filtered in the encoder driver (main-loop diagnose()); near zero at
 * standstill.
 */
float platform_get_motor_rpm(void);

/**
 * @brief Latest DC-link voltage [V].
 */
float platform_get_dc_link_voltage(void);

/**
 * @brief Mechanical speed [RPM] from the encoder.
 */
float platform_get_rpm_mech(void);

/**
 * @brief Electrical speed [RPM] = mech RPM * pole pairs.
 */
float platform_get_rpm_elec(void);

/**
 * @brief Raw encoder sin/cos ADC counts (for nonlinearity diagnosis).
 */
uint32_t platform_get_encoder_raw_sin(void);
uint32_t platform_get_encoder_raw_cos(void);

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

/* Supplemental high-rate trace.  This does not use or modify Telemetry. */
void platform_trace_configure8(
    const char* key0, float scale0, const char* key1, float scale1,
    const char* key2, float scale2, const char* key3, float scale3,
    const char* key4, float scale4, const char* key5, float scale5,
    const char* key6, float scale6, const char* key7, float scale7);
void platform_trace_capture8(float value0, float value1, float value2, float value3,
                             float value4, float value5, float value6, float value7);
bool platform_trace_register_event(uint8_t channel, const char* key);
bool platform_trace_event(uint8_t channel, float value, bool snapshot);

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

/**
 * @brief Set/get the time step of the currently executing generated domain.
 *
 * The base image calls platform_set_current_domain_dt() before invoking each
 * generated domain's step function.  Nodes that need a time step should read
 * platform_get_current_domain_dt() instead of using a manual Dt parameter.
 */
void platform_set_current_domain_dt(float dt_s);
float platform_get_current_domain_dt(void);

#ifdef __cplusplus
}
#endif
