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

/* Sensed phase currents [A] from the PWM-synchronous ADC model (Gen6
 * PhaseCurrentADC::sample semantics): the inverted-wiring recovery of the
 * latched injected channels minus the calibrated zero offset, with W
 * reconstructed as -(U + V). Always reflects the latest conversion; true
 * whenever a plant is present. */
bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a);

/* Current observer (Gen6FW Inverter::CurrentObserver; sim port lives in
 * src/current_observer.cpp and keeps the same predict/correct math).
 *
 * platform_observer_init_from_calibration seeds R/L/flux from the scenario
 * "motor" parameters (rs_ohm, ld_h, flux_wb, pole_pairs) — the sim's
 * equivalent of the Gen6 MotorCalibration snapshot — and resets the state,
 * exactly like Gen6's platform_api.cpp.  The runtime also applies those
 * parameters once at domain init, so a graph that only drives
 * predict/correct still sees plausible defaults.  The predict step uses
 * platform_get_current_domain_dt() just like on hardware (the scheduler now
 * sets it per domain, see below).
 *
 * platform_set/get_use_observer is pure flag storage, as on Gen6: the flag
 * does not change what the observer computes — generated graph code (or the
 * native FOC on hardware) decides whether to consume observer currents.
 * (e.g. Gen6 FocControlManager reads m_use_observer to pick its feedback;
 * a graph does the same with a gate node calling platform_get_use_observer.)
 *
 * As in Gen6, platform_get_observer_currents is only meaningful after at
 * least one platform_observer_correct call.  All quantities use FOC
 * convention: graphs feed the sign-corrected currents (the sim models the
 * hardware's inverted sensor wiring, graphs negate as usual). */
void platform_set_use_observer(bool enabled);
bool platform_get_use_observer(void);
void platform_get_observer_currents(float* iu_a, float* iv_a, float* iw_a);
void platform_observer_predict(float valpha_v, float vbeta_v,
                               float theta_elec_rad, float dt_s);
void platform_observer_set_motor_params(float r_ohm, float l_henry,
                                        float flux_linkage_wb,
                                        float pole_pairs);
void platform_observer_init_from_calibration(void);
void platform_observer_correct(float iu_meas_a, float iv_meas_a,
                               float diudt_a_per_s, float divdt_a_per_s,
                               uint32_t t_us);

/* Phase-current ADC injected channels, mirroring Gen6FW's PhaseCurrentADC
 * signal chain (16-bit ADC, resistor divider, current transducer with a
 * ~1.65 V zero-current reference rail; constants in RteParams.h), including
 * the hardware's inverted sensor wiring (graphs fix the sign with their
 * InvertPolarity parameter, as on Gen6):
 *
 *   *_sig = reference counts - phase_current * counts_per_amp (rail-saturated)
 *   *_ref = reference rail counts
 *   recovered A = (sig - ref) * (vref / 2^bits) / (divider * sensitivity)
 *
 * Samples latch per conversion trigger (each adc_isr tick or an explicit
 * platform_sample_application_sensors call). Scenario "adc" keys inject
 * resolution/reference/gain/offset/noise error terms; the offset getters
 * report the calibrated zero-offset in recovered amps (i.e. -(injected
 * bias), matching Gen6's standstill calibration). */
uint32_t platform_adc_get_injected_u_sig(void);
uint32_t platform_adc_get_injected_v_sig(void);
uint32_t platform_adc_get_injected_u_ref(void);
uint32_t platform_adc_get_injected_v_ref(void);
float platform_adc_get_offset_u_a(void);
float platform_adc_get_offset_v_a(void);

/* Latest phase-current micro-burst [A], Gen6FW PhaseCurrentADC::BurstSample
 * layout: two points per phase (ranks 1/2 and 3/4 of the injected sequence).
 * Both points come from the same latched conversion (the sim models no
 * intra-burst droop, so iu1 == iu0 etc.). Currents are recovered with the
 * same (sig - ref) * lsb / (divider * sensitivity) chain and the calibrated
 * zero offset is subtracted, exactly like the Gen6 firmware path.
 * time_us receives the sim timestamp of the latched conversion (may be
 * nullptr). Returns false on null current outputs; valid only after a
 * conversion (each adc_isr tick triggers one). */
bool platform_adc_get_burst_sample(float* iu0_a, float* iv0_a,
                                   float* iu1_a, float* iv1_a,
                                   uint32_t* time_us);

/* TIM1 auto-reload value from the Gen6 hardware configuration (275 MHz timer
 * clock, center-aligned -> ARR 27500, Src/tim.c). Constant in the sim; the
 * graph only uses it to scale duty cycle into timer ticks. */
uint32_t platform_pwm_get_arr(void);

/* Adaptive phase-current sample scheduling, port of Gen6FW
 * PWM_FindSafeSamplePoint + wrapper: converts duties to CCR ticks against
 * arr, finds the widest all-low / all-high quiet window in the center-aligned
 * PWM period, requires at least 1650 ticks (~6 us at 275 MHz).
 * Returns the largest gap in timer ticks, 0 when the legacy bottom-trigger
 * fallback would be used. The sim's ADC conversions stay adc_isr tick-driven
 * regardless; the CCR4 side effect does not exist here. */
uint32_t platform_schedule_adaptive_sample(float duty_u, float duty_v,
                                           float duty_w, uint32_t arr);

/* Encoder angle as the Gen6 sin/cos encoder driver reports it: MECHANICAL
 * degrees in [0, 360) — one sin/cos cycle per mechanical revolution. The sim
 * plant integrates the electrical angle, so the mechanical angle is derived
 * as theta_e / pole_pairs (mod 360). The graph's Transforms.ElecAngle node
 * re-applies the pole-pair factor, exactly as FocController does on
 * hardware. platform_get_encoder_angle returns true only when a new sample
 * arrived since the last call (the value is still written either way). */
bool platform_get_encoder_angle(float* angle_deg);
float platform_get_encoder_angle_latest(void);
/* Mechanical speed [rpm], signed by direction. Both rpm getters read the
 * same plant state; the sim reports the exact plant speed instead of the
 * Gen6 encoder driver's EMA-windowed estimate. */
float platform_get_motor_rpm(void);
float platform_get_rpm_mech(void);
/* Electrical speed [rpm] = mech rpm * pole pairs * encoder sign (Gen6
 * platform_get_rpm_elec: rpmMech * pole_pairs * MotorCalibration sign).
 * The simulated encoder counts in the positive rotation direction, so the
 * encoder sign is +1. */
float platform_get_rpm_elec(void);
/* Control and display speed both use the exact plant state in HostSim. */
float platform_get_control_rpm_mech(void);
float platform_get_control_rpm_elec(void);
/* Raw encoder sin/cos ADC counts (Gen6 EncoderADC::lastRawSin/lastRawCos).
 * The mechanical angle is rendered as a 16-bit ADC sinusoid: center 32768,
 * amplitude 30000 (one cycle per mechanical revolution, inside the driver's
 * 427..65388 hard caps — same model as HostSIL's sil_encoder_adc.cpp).
 * atan2 of the decoded pair reproduces platform_get_encoder_angle_latest()
 * up to count quantization. */
uint32_t platform_get_encoder_raw_sin(void);
uint32_t platform_get_encoder_raw_cos(void);
float platform_get_dc_link_voltage(void);
/* Plant readback: terminal voltage the plant actually applied last step
 * (volt vs DC-), not the requested duty. */
float platform_phase_voltage_u(void);
float platform_phase_voltage_v(void);
float platform_phase_voltage_w(void);
float platform_get_throttle_a(void);
float platform_get_throttle_b(void);
bool platform_get_throttle_valid(void);
/* Scenario-driven ("environment" object); defaults 25 C. */
float platform_get_motor_temperature(void);
float platform_get_inverter_temperature(uint8_t channel);

/* Digital IO loopback: reads see the last written level per pin. */
bool platform_digital_read(uint8_t pin);
void platform_digital_write(uint8_t pin, bool value);

/* CAN bus (bus 1 = A, 2 = B). Latest-frame store keyed by (bus, id): sent
 * frames are readable via platform_can_rx when loopback is on (scenario "can"
 * object, default true); scenario "frames" entries can also inject traffic.
 * platform_can_rx returns the DLC (0..8) or -1 when no frame arrived. */
bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                       const uint8_t* data, uint8_t dlc);
int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out);
/* Slow-channel conversion trigger; also refreshes the ADC sample latch. */
void platform_sample_application_sensors(void);

void platform_raise_fault(uint32_t source, uint8_t reason);
bool platform_has_critical_fault(void);

/* Critical sections (recursive mutex guards shared sim/config state). */
void platform_critical_enter(void);
void platform_critical_exit(void);

/* Runtime config store. In-memory by default; scenario key
 * "config_file" additionally persists key=value lines to a file, preloaded at
 * startup and flushed on every set/load-created default. */
float platform_config_load(const char* key, float default_value);
void platform_config_set(const char* key, float value);
float platform_config_get(const char* key);

void platform_telemetry_log_f32(const char* key, float value);

uint32_t platform_millis(void);
uint32_t platform_micros(void);

/* Time step of the currently executing generated domain. Mirrors the Gen6
 * ISR wrappers (pwm.cpp / PhaseCurrentADC.cpp / InverterMain.cpp): the
 * scheduler calls platform_set_current_domain_dt() with the domain's step
 * right before each generated domain step, so the getter returns 1/tim_isr_hz
 * inside tim_isr nodes, 1/adc_isr_hz inside adc_isr nodes, and 1/app_loop_hz
 * inside app_loop nodes.  hw.current_observer uses it for the prediction dt;
 * nodes that need a step size outside a domain step should still carry a Dt
 * parameter. */
void platform_set_current_domain_dt(float dt_s);
float platform_get_current_domain_dt(void);

#ifdef __cplusplus
}
#endif
