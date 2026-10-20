/**
 * C++ host shim implementing the platform_api.h contract of the Gen6FW base
 * image, binding emitted RTE graph code (foc_demo) to the generated plant
 * (PlantCodegen contract) or the built-in fallback plant.
 *
 * Ported from twin/native/src/platform_shim.cpp; measurements come from the
 * period-fresh plant::PlantMeas snapshot instead of twin::PlantPmsm::meas,
 * and the duty sink stays a latch (the scheduler pushes it into the plant
 * via plant::PlantSetDuty once per control period).
 *
 * The meas fields are accessed through C++17 SFINAE detection so the shim
 * compiles against any plant_generated.h honoring the contract: fields the
 * graph does not connect (e.g. phase voltages) degrade to the same safe
 * defaults the monolithic native shim reported.
 *
 * Firmware conventions replicated here (verbatim from twin/native):
 *  - burst phase-current samples are in the hardware's sensor polarity
 *    (graph negates them; Custom.PhaseCurrentsBurst inline.cpp), so we feed
 *    -iu_a/-iv_a;
 *  - encoder decode normalizes counts onto the learned healthy bounds, one
 *    sin/cos cycle per mechanical rev ([deg, 0..360));
 *  - platform_config_load with an unknown key creates it with the default
 *    (FRAM create semantics).
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

#include "platform_api.h"
#include "plant_generated.h"
#include "meas_access.h"
#include "shim_runtime.h"

RTESIM_MEAS_ACCESSOR(Iu_A)
RTESIM_MEAS_ACCESSOR(Iv_A)
RTESIM_MEAS_ACCESSOR(Iw_A)
RTESIM_MEAS_ACCESSOR(Vdc_V)
RTESIM_MEAS_ACCESSOR(Enc_Sin)
RTESIM_MEAS_ACCESSOR(Enc_Cos)
RTESIM_MEAS_ACCESSOR(Temp_Inv1_C)
RTESIM_MEAS_ACCESSOR(Temp_Inv2_C)
RTESIM_MEAS_ACCESSOR(Temp_Inv3_C)
RTESIM_MEAS_ACCESSOR(Temp_Motor_C)
RTESIM_MEAS_ACCESSOR(Mech_Rpm)
RTESIM_MEAS_ACCESSOR(Elec_Rpm)
RTESIM_MEAS_ACCESSOR(Dclink_I_A)
RTESIM_MEAS_ACCESSOR(Dclink_P_W)
RTESIM_MEAS_ACCESSOR(Vu_V)
RTESIM_MEAS_ACCESSOR(Vv_V)
RTESIM_MEAS_ACCESSOR(Vw_V)

namespace {

rtesim_host::ShimRuntime& S() { return rtesim_host::ShimRuntime::instance(); }
constexpr double kPi = 3.14159265358979323846;

// Null-checked snapshot accessors (fallback when the scheduler has not
// published a measurement yet / the field is absent from this PlantMeas).
double meas_Iu_A(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Iu_A(*m, f) : f; }
double meas_Iv_A(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Iv_A(*m, f) : f; }
double meas_Iw_A(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Iw_A(*m, f) : f; }
double meas_Vdc_V(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Vdc_V(*m, f) : f; }
double meas_Enc_Sin(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Enc_Sin(*m, f) : f; }
double meas_Enc_Cos(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Enc_Cos(*m, f) : f; }
double meas_Temp_Inv1_C(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Temp_Inv1_C(*m, f) : f; }
double meas_Temp_Inv2_C(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Temp_Inv2_C(*m, f) : f; }
double meas_Temp_Inv3_C(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Temp_Inv3_C(*m, f) : f; }
double meas_Temp_Motor_C(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Temp_Motor_C(*m, f) : f; }
double meas_Mech_Rpm(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Mech_Rpm(*m, f) : f; }
double meas_Elec_Rpm(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Elec_Rpm(*m, f) : f; }
double meas_Dclink_I_A(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Dclink_I_A(*m, f) : f; }
double meas_Dclink_P_W(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Dclink_P_W(*m, f) : f; }
double meas_Vu_V(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Vu_V(*m, f) : f; }
double meas_Vv_V(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Vv_V(*m, f) : f; }
double meas_Vw_V(double f) { const plant::PlantMeas* m = S().meas; return m ? Meas_Vw_V(*m, f) : f; }

} // namespace

/* --------------------------------------------------------------------------
 * PWM / gate-driver outputs
 * -------------------------------------------------------------------------- */

extern "C" void platform_pwm_set(float du, float dv, float dw) {
    S().pwm_du = du;
    S().pwm_dv = dv;
    S().pwm_dw = dw;
    // the scheduler latches these into plant::PlantSetDuty once per control
    // period (the plant graph freezes the duty vector over its substeps,
    // like the monolithic twin's per-400us semantics)
}

extern "C" void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    (void)valpha; (void)vbeta; (void)vdc;   // unused by foc_demo graph
}

extern "C" uint32_t platform_pwm_get_arr(void) {
    return 27500;   // TIM1 @275 MHz center-aligned (pack power_stage note)
}

extern "C" uint32_t platform_schedule_adaptive_sample(float duty_u, float duty_v,
                                                      float duty_w, uint32_t arr) {
    (void)duty_u; (void)duty_v; (void)duty_w; (void)arr;
    return 0;   // v0: fallback trigger (GapTicks=0, matching the python twin)
}

/* --------------------------------------------------------------------------
 * Sensor inputs
 * -------------------------------------------------------------------------- */

extern "C" bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (!S().plant || !iu_a || !iv_a || !iw_a) return false;
    *iu_a = static_cast<float>(meas_Iu_A(0.0));
    *iv_a = static_cast<float>(meas_Iv_A(0.0));
    *iw_a = static_cast<float>(meas_Iw_A(0.0));
    return true;
}

extern "C" bool platform_adc_get_burst_sample(float* iu0_a, float* iv0_a,
                                              float* iu1_a, float* iv1_a,
                                              uint32_t* time_us) {
    // Hardware sensor polarity is inverted relative to the FOC convention
    // (generated PhaseCurrentsBurst node negates), and W is reconstructed
    // downstream, so feed negated U/V quantized samples, both ranks equal.
    if (!S().plant) return false;
    const float iu = -static_cast<float>(meas_Iu_A(0.0));
    const float iv = -static_cast<float>(meas_Iv_A(0.0));
    *iu0_a = iu;
    *iu1_a = iu;
    *iv0_a = iv;
    *iv1_a = iv;
    *time_us = static_cast<uint32_t>(S().now_us & 0xFFFFFFFFu);
    return true;
}

extern "C" void platform_set_use_observer(bool enabled) { (void)enabled; }
extern "C" bool platform_get_use_observer(void) { return false; }
extern "C" void platform_get_observer_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (iu_a) *iu_a = 0.0f;
    if (iv_a) *iv_a = 0.0f;
    if (iw_a) *iw_a = 0.0f;
}
extern "C" void platform_observer_predict(float valpha_v, float vbeta_v,
                                          float theta_elec_rad, float dt_s) {
    (void)valpha_v; (void)vbeta_v; (void)theta_elec_rad; (void)dt_s;
}
extern "C" void platform_observer_set_motor_params(float r_ohm, float l_henry,
                                                   float flux_linkage_wb, float pole_pairs) {
    (void)r_ohm; (void)l_henry; (void)flux_linkage_wb; (void)pole_pairs;
}
extern "C" void platform_observer_init_from_calibration(void) {}
extern "C" void platform_observer_correct(float iu_meas_a, float iv_meas_a,
                                          float diudt_a_per_s, float divdt_a_per_s,
                                          uint32_t t_us) {
    (void)iu_meas_a; (void)iv_meas_a; (void)diudt_a_per_s; (void)divdt_a_per_s; (void)t_us;
}

extern "C" uint32_t platform_adc_get_injected_u_sig(void) { return 0; }
extern "C" uint32_t platform_adc_get_injected_v_sig(void) { return 0; }
extern "C" uint32_t platform_adc_get_injected_u_ref(void) { return 0; }
extern "C" uint32_t platform_adc_get_injected_v_ref(void) { return 0; }
extern "C" float platform_adc_get_offset_u_a(void) { return 0.0f; }
extern "C" float platform_adc_get_offset_v_a(void) { return 0.0f; }

namespace {
// counts -> normalized mechanical angle [deg, 0..360) on the learned healthy
// bounds (same normalization the python replica applies in FocController.step;
// bounds from the Encoder-Loss report, see meas_access.h)
float encoder_angle_deg() {
    if (!S().plant) return 0.0f;
    const double ns = (meas_Enc_Sin(rtesim_host::kEncMidSin) - rtesim_host::kEncMidSin)
                      / rtesim_host::kEncAmpSin;
    const double nc = (meas_Enc_Cos(rtesim_host::kEncMidCos) - rtesim_host::kEncMidCos)
                      / rtesim_host::kEncAmpCos;
    double th = std::atan2(ns, nc);
    if (th < 0.0) th += 2.0 * kPi;
    return static_cast<float>(th * 180.0 / kPi);
}
} // namespace

extern "C" bool platform_get_encoder_angle(float* angle_deg) {
    if (!angle_deg) return false;
    *angle_deg = encoder_angle_deg();
    return true;
}

extern "C" float platform_get_encoder_angle_latest(void) {
    return encoder_angle_deg();
}

extern "C" float platform_get_motor_rpm(void) {
    return static_cast<float>(meas_Mech_Rpm(0.0));
}

extern "C" float platform_get_rpm_mech(void) {
    return static_cast<float>(meas_Mech_Rpm(0.0));
}

extern "C" float platform_get_rpm_elec(void) {
    return static_cast<float>(meas_Elec_Rpm(0.0));
}

extern "C" float platform_get_dc_link_voltage(void) {
    return static_cast<float>(meas_Vdc_V(0.0));
}

extern "C" uint32_t platform_get_encoder_raw_sin(void) {
    return static_cast<uint32_t>(meas_Enc_Sin(0.0));
}

extern "C" uint32_t platform_get_encoder_raw_cos(void) {
    return static_cast<uint32_t>(meas_Enc_Cos(0.0));
}

extern "C" float platform_get_dc_link_current(void) {
    return static_cast<float>(meas_Dclink_I_A(0.0));
}

extern "C" float platform_get_dc_link_power(void) {
    return static_cast<float>(meas_Dclink_P_W(0.0));
}

/* MAX22530 phase-voltage channels: the averaged plant graph may wire them
 * (Vu_V/Vv_V/Vw_V on MeasOut); when wire-free report 0 like the monolithic
 * shim did, and let the vsense domain log it. */
extern "C" float platform_phase_voltage_u(void) {
    return static_cast<float>(meas_Vu_V(0.0));
}
extern "C" float platform_phase_voltage_v(void) {
    return static_cast<float>(meas_Vv_V(0.0));
}
extern "C" float platform_phase_voltage_w(void) {
    return static_cast<float>(meas_Vw_V(0.0));
}

extern "C" float platform_get_throttle_a(void) { return 0.0f; }
extern "C" float platform_get_throttle_b(void) { return 0.0f; }
extern "C" bool platform_get_throttle_valid(void) { return false; }

extern "C" float platform_get_motor_temperature(void) {
    return static_cast<float>(meas_Temp_Motor_C(NAN));  // null channel, as python twin
}

extern "C" float platform_get_inverter_temperature(uint8_t channel) {
    if (!S().plant) return NAN;
    switch (channel) {
        case 0: return static_cast<float>(meas_Temp_Inv1_C(NAN));
        case 1: return static_cast<float>(meas_Temp_Inv2_C(NAN));
        case 2: return static_cast<float>(meas_Temp_Inv3_C(NAN));
        default: return NAN;   // TS channels beyond the fitted ones are null
    }
}

/* --------------------------------------------------------------------------
 * User digital IO / CAN / trace - unused by foc_demo
 * -------------------------------------------------------------------------- */

extern "C" bool platform_digital_read(uint8_t pin) { (void)pin; return false; }
extern "C" void platform_digital_write(uint8_t pin, bool value) {
    (void)pin; (void)value;
}

extern "C" bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                                  const uint8_t* data, uint8_t dlc) {
    (void)bus; (void)id; (void)ext; (void)data; (void)dlc;
    return false;
}

extern "C" int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out) {
    (void)bus; (void)id; (void)data; (void)seq_out;
    return -1;
}

extern "C" void platform_trace_configure8(
    const char* key0, float scale0, const char* key1, float scale1,
    const char* key2, float scale2, const char* key3, float scale3,
    const char* key4, float scale4, const char* key5, float scale5,
    const char* key6, float scale6, const char* key7, float scale7) {
    (void)key0; (void)scale0; (void)key1; (void)scale1;
    (void)key2; (void)scale2; (void)key3; (void)scale3;
    (void)key4; (void)scale4; (void)key5; (void)scale5;
    (void)key6; (void)scale6; (void)key7; (void)scale7;
}

extern "C" void platform_trace_capture8(float value0, float value1, float value2, float value3,
                                        float value4, float value5, float value6, float value7) {
    (void)value0; (void)value1; (void)value2; (void)value3;
    (void)value4; (void)value5; (void)value6; (void)value7;
}

extern "C" bool platform_trace_register_event(uint8_t channel, const char* key) {
    (void)channel; (void)key;
    return false;
}

extern "C" bool platform_trace_event(uint8_t channel, float value, bool snapshot) {
    (void)channel; (void)value; (void)snapshot;
    return false;
}

/* --------------------------------------------------------------------------
 * Safety / faults (v0: no fault propagation back into the firmware)
 * -------------------------------------------------------------------------- */

extern "C" void platform_raise_fault(uint32_t source, uint8_t reason) {
    (void)source; (void)reason;
}

extern "C" bool platform_has_critical_fault(void) { return false; }

/* --------------------------------------------------------------------------
 * Critical sections: single-threaded scheduler, no preemption
 * -------------------------------------------------------------------------- */

extern "C" void platform_critical_enter(void) {}
extern "C" void platform_critical_exit(void) {}

/* --------------------------------------------------------------------------
 * Config / persistence (FRAM-seeded store)
 * -------------------------------------------------------------------------- */

extern "C" float platform_config_load(const char* key, float default_value) {
    auto& cfg = S().config;
    auto it = cfg.find(key);
    if (it != cfg.end()) return it->second;
    cfg[key] = default_value;   // FRAM create semantics
    return default_value;
}

extern "C" void platform_config_set(const char* key, float value) {
    S().config[key] = value;
}

extern "C" float platform_config_get(const char* key) {
    auto it = S().config.find(key);
    return it != S().config.end() ? it->second : 0.0f;
}

/* --------------------------------------------------------------------------
 * Telemetry: latest value per key; the scheduler publishes frames at the
 * configured telemetry rate
 * -------------------------------------------------------------------------- */

extern "C" void platform_telemetry_log_f32(const char* key, float value) {
    S().telemetry[key] = value;
}

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

extern "C" uint32_t platform_millis(void) {
    return static_cast<uint32_t>((S().now_us / 1000ull) & 0xFFFFFFFFu);
}

extern "C" uint32_t platform_micros(void) {
    return static_cast<uint32_t>(S().now_us & 0xFFFFFFFFu);
}

extern "C" void platform_set_current_domain_dt(float dt_s) {
    S().domain_dt = dt_s;
}

extern "C" float platform_get_current_domain_dt(void) {
    return S().domain_dt;
}
