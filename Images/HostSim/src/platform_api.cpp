#include "platform_api.h"

#include "motor_model.h"
#include "sim_context.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hostsim {

MotorModel* g_motor = nullptr;

namespace {
std::mutex g_cfg_mu;
std::unordered_map<std::string, float> g_config;
} // namespace

SimContext g_sim_ctx{};

SimContext& GetSimContext() { return g_sim_ctx; }

void SimNotifyEncoderSample() { g_sim_ctx.encoder_sample_new = true; }

void SimRuntime_RegisterMotor(MotorModel* motor) { g_motor = motor; }

} // namespace hostsim

extern "C" {

void platform_pwm_set(float du, float dv, float dw) {
    auto& c = hostsim::GetSimContext();
    c.duty_u = du;
    c.duty_v = dv;
    c.duty_w = dw;
}

void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    const float v_max = vdc / std::sqrt(3.0f);
    const float mag = std::sqrt(valpha * valpha + vbeta * vbeta);
    float scale = 1.0f;
    if (mag > v_max && mag > 1e-6f) scale = v_max / mag;

    const float va = valpha * scale;
    const float vb = -0.5f * valpha * scale + 0.8660254f * vbeta * scale;
    const float vc = -0.5f * valpha * scale - 0.8660254f * vbeta * scale;

    const float du = std::max(0.0f, std::min(100.0f, 50.0f + 50.0f * va / vdc));
    const float dv = std::max(0.0f, std::min(100.0f, 50.0f + 50.0f * vb / vdc));
    const float dw = std::max(0.0f, std::min(100.0f, 50.0f + 50.0f * vc / vdc));
    platform_pwm_set(du, dv, dw);
}

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (!hostsim::g_motor) return false;
    const auto& st = hostsim::g_motor->State();
    if (iu_a) *iu_a = st.ia_a;
    if (iv_a) *iv_a = st.ib_a;
    if (iw_a) *iw_a = st.ic_a;
    return true;
}

uint32_t platform_adc_get_injected_u_sig(void) { return 0; }
uint32_t platform_adc_get_injected_v_sig(void) { return 0; }
uint32_t platform_adc_get_injected_u_ref(void) { return 0; }
uint32_t platform_adc_get_injected_v_ref(void) { return 0; }
float platform_adc_get_offset_u_a(void) { return 0.0f; }
float platform_adc_get_offset_v_a(void) { return 0.0f; }

bool platform_get_encoder_angle(float* angle_deg) {
    if (!hostsim::g_motor) return false;
    if (angle_deg) *angle_deg = hostsim::g_motor->ThetaElectricalDeg();
    auto& ctx = hostsim::GetSimContext();
    const bool had = ctx.encoder_sample_new;
    ctx.encoder_sample_new = false;
    return had;
}

float platform_get_encoder_angle_latest(void) {
    if (!hostsim::g_motor) return 0.0f;
    return hostsim::g_motor->ThetaElectricalDeg();
}

float platform_get_dc_link_voltage(void) {
    return hostsim::GetSimContext().vdc_v;
}

float platform_get_throttle_a(void) {
    return hostsim::GetSimContext().throttle_a;
}

float platform_get_throttle_b(void) {
    return hostsim::GetSimContext().throttle_b;
}

float platform_get_motor_temperature(void) { return 25.0f; }
float platform_get_inverter_temperature(uint8_t channel) {
    (void)channel;
    return 25.0f;
}

void platform_sample_application_sensors(void) {}

void platform_raise_fault(uint32_t source, uint8_t reason) {
    (void)source;
    (void)reason;
    hostsim::GetSimContext().critical_fault = true;
}

bool platform_has_critical_fault(void) {
    return hostsim::GetSimContext().critical_fault;
}

void platform_critical_enter(void) {}
void platform_critical_exit(void) {}

float platform_config_load(const char* key, float default_value) {
    if (!key) return default_value;
    std::lock_guard<std::mutex> lock(hostsim::g_cfg_mu);
    auto it = hostsim::g_config.find(key);
    if (it != hostsim::g_config.end()) return it->second;
    hostsim::g_config[key] = default_value;
    return default_value;
}

void platform_config_set(const char* key, float value) {
    if (!key) return;
    std::lock_guard<std::mutex> lock(hostsim::g_cfg_mu);
    hostsim::g_config[key] = value;
}

float platform_config_get(const char* key) {
    if (!key) return 0.0f;
    std::lock_guard<std::mutex> lock(hostsim::g_cfg_mu);
    auto it = hostsim::g_config.find(key);
    return it != hostsim::g_config.end() ? it->second : 0.0f;
}

void platform_telemetry_log_f32(const char* key, float value) {
    if (!key) return;
    std::fprintf(stderr, "telemetry %s=%g\n", key, static_cast<double>(value));
}

uint32_t platform_millis(void) {
    return static_cast<uint32_t>(hostsim::GetSimContext().time_us / 1000ULL);
}

uint32_t platform_micros(void) {
    return static_cast<uint32_t>(hostsim::GetSimContext().time_us);
}

} // extern "C"
