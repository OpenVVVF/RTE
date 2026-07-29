#pragma once

/**
 * @brief Slim platform API exposed to RTE-generated code (HostSim).
 *
 * Signatures match the Gen6FW / NucleoL476FW platform_api.h subset so graphs
 * built against upstream node templates keep working.  Simulator behaviour
 * lives in src/platform_api.cpp behind these calls.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void platform_pwm_set(float du, float dv, float dw);
void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc);

/* Open-loop sinusoidal PWM (SPWM) helper for graph demos.
 * Advances an internal electrical angle each call and writes phase duties in %. */
void platform_spwm_step(float modulation_index, float electrical_freq_hz, float dt_s,
                        float* duty_u, float* duty_v, float* duty_w);
float platform_spwm_get_angle_rad(void);
float platform_spwm_get_angle_deg(void);
void platform_spwm_reset(void);

/* Switched PWM scope outputs (triangle carrier vs duty command). */
float platform_pwm_scope_get_gate_u(void);
float platform_pwm_scope_get_gate_v(void);
float platform_pwm_scope_get_gate_w(void);
float platform_pwm_scope_get_v_u(void);
float platform_pwm_scope_get_v_v(void);
float platform_pwm_scope_get_v_w(void);
float platform_pwm_scope_get_v_uv(void);
float platform_pwm_scope_get_v_vw(void);
float platform_pwm_scope_get_v_wu(void);

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a);

uint32_t platform_adc_get_injected_u_sig(void);
uint32_t platform_adc_get_injected_v_sig(void);
uint32_t platform_adc_get_injected_u_ref(void);
uint32_t platform_adc_get_injected_v_ref(void);
float platform_adc_get_offset_u_a(void);
float platform_adc_get_offset_v_a(void);

bool platform_get_encoder_angle(float* angle_deg);
float platform_get_encoder_angle_latest(void);
float platform_get_dc_link_voltage(void);
float platform_get_throttle_a(void);
float platform_get_throttle_b(void);
float platform_get_motor_temperature(void);
float platform_get_inverter_temperature(uint8_t channel);
void platform_sample_application_sensors(void);

void platform_raise_fault(uint32_t source, uint8_t reason);
bool platform_has_critical_fault(void);

void platform_critical_enter(void);
void platform_critical_exit(void);

float platform_config_load(const char* key, float default_value);
void platform_config_set(const char* key, float value);
float platform_config_get(const char* key);

void platform_telemetry_log_f32(const char* key, float value);

uint32_t platform_millis(void);
uint32_t platform_micros(void);

#ifdef __cplusplus
}
#endif
