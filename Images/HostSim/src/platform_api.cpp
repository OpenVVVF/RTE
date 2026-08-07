#include "platform_api.h"

#include "motor_model.h"
#include "pwm_scope.h"
#include "sim_context.h"
#include "telemetry_publisher.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(_MSC_VER)
#include <stdlib.h>
#endif
#include <mutex>
#include <string>
#include <unordered_map>

#include "plant/plant_backend.h"
#include "plant/ode_plant.h"

namespace hostsim {

IPlant* g_plant = nullptr;
MotorModel* g_motor = nullptr;

namespace {
std::mutex g_cfg_mu;
std::unordered_map<std::string, float> g_config;

struct SpwmState {
    float angle_rad = 0.0f;
};

SpwmState g_spwm;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kPhase120Rad = 2.09439510239f;
} // namespace

SimContext g_sim_ctx{};

SimContext& GetSimContext() { return g_sim_ctx; }

void SimNotifyEncoderSample() { g_sim_ctx.encoder_sample_new = true; }

int g_active_pole_pairs = 7;

void SimRuntime_RegisterPlant(IPlant* plant) {
    g_plant = plant;
    auto* ode = dynamic_cast<OdePlant*>(plant);
    if (ode) {
        g_motor = &ode->Model();
        g_active_pole_pairs = ode->Model().Params().pole_pairs;
    } else {
        g_motor = nullptr;
    }
}

void SimRuntime_RegisterMotor(MotorModel* motor) {
    g_motor = motor;
    if (motor) {
        g_active_pole_pairs = motor->Params().pole_pairs;
    }
}

} // namespace hostsim

extern "C" {

void platform_pwm_set(float du, float dv, float dw) {
    auto& c = hostsim::GetSimContext();
    c.duty_u = du;
    c.duty_v = dv;
    c.duty_w = dw;
    hostsim::GlobalPwmScope().SetDuties(du, dv, dw);
}

void platform_spwm_step(float modulation_index, float electrical_freq_hz, float dt_s,
                        float* duty_u, float* duty_v, float* duty_w) {
    float m = modulation_index;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;

    hostsim::g_spwm.angle_rad += hostsim::kTwoPi * electrical_freq_hz * dt_s;
    while (hostsim::g_spwm.angle_rad >= hostsim::kTwoPi) {
        hostsim::g_spwm.angle_rad -= hostsim::kTwoPi;
    }

    const float angle = hostsim::g_spwm.angle_rad;
    const float u = m * std::sin(angle);
    const float v = m * std::sin(angle - hostsim::kPhase120Rad);
    const float w = m * std::sin(angle + hostsim::kPhase120Rad);

    float du = 50.0f + 50.0f * u;
    float dv = 50.0f + 50.0f * v;
    float dw = 50.0f + 50.0f * w;
    du = std::max(0.0f, std::min(100.0f, du));
    dv = std::max(0.0f, std::min(100.0f, dv));
    dw = std::max(0.0f, std::min(100.0f, dw));

    if (duty_u) *duty_u = du;
    if (duty_v) *duty_v = dv;
    if (duty_w) *duty_w = dw;
}

float platform_spwm_get_angle_rad(void) { return hostsim::g_spwm.angle_rad; }

float platform_spwm_get_angle_deg(void) {
    return hostsim::g_spwm.angle_rad * 57.2957795131f;
}

void platform_spwm_reset(void) { hostsim::g_spwm.angle_rad = 0.0f; }

float platform_pwm_scope_get_gate_u(void) {
    return hostsim::GlobalPwmScope().GateU();
}
float platform_pwm_scope_get_gate_v(void) {
    return hostsim::GlobalPwmScope().GateV();
}
float platform_pwm_scope_get_gate_w(void) {
    return hostsim::GlobalPwmScope().GateW();
}
float platform_pwm_scope_get_v_u(void) { return hostsim::GlobalPwmScope().VoltageU(); }
float platform_pwm_scope_get_v_v(void) { return hostsim::GlobalPwmScope().VoltageV(); }
float platform_pwm_scope_get_v_w(void) { return hostsim::GlobalPwmScope().VoltageW(); }
float platform_pwm_scope_get_v_uv(void) { return hostsim::GlobalPwmScope().VoltageUV(); }
float platform_pwm_scope_get_v_vw(void) { return hostsim::GlobalPwmScope().VoltageVW(); }
float platform_pwm_scope_get_v_wu(void) { return hostsim::GlobalPwmScope().VoltageWU(); }

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
    if (hostsim::g_plant) {
        const auto& st = hostsim::g_plant->State();
        if (iu_a) *iu_a = st.ia_a;
        if (iv_a) *iv_a = st.ib_a;
        if (iw_a) *iw_a = st.ic_a;
        return true;
    }
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
    if (hostsim::g_plant) {
        if (angle_deg) *angle_deg = hostsim::g_plant->ThetaElectricalDeg();
        auto& ctx = hostsim::GetSimContext();
        const bool had = ctx.encoder_sample_new;
        ctx.encoder_sample_new = false;
        return had;
    }
    if (!hostsim::g_motor) return false;
    if (angle_deg) *angle_deg = hostsim::g_motor->ThetaElectricalDeg();
    auto& ctx = hostsim::GetSimContext();
    const bool had = ctx.encoder_sample_new;
    ctx.encoder_sample_new = false;
    return had;
}

float platform_get_encoder_angle_latest(void) {
    if (hostsim::g_plant) return hostsim::g_plant->ThetaElectricalDeg();
    if (!hostsim::g_motor) return 0.0f;
    return hostsim::g_motor->ThetaElectricalDeg();
}

float platform_get_motor_rpm(void) {
    float omega_e = 0.0f;
    int pole_pairs = hostsim::g_active_pole_pairs;
    if (hostsim::g_motor) {
        pole_pairs = hostsim::g_motor->Params().pole_pairs;
    }
    if (hostsim::g_plant) {
        omega_e = hostsim::g_plant->OmegaElectricalRadPerSec();
    } else if (hostsim::g_motor) {
        omega_e = hostsim::g_motor->OmegaElectricalRadPerSec();
    } else {
        return 0.0f;
    }
    if (pole_pairs <= 0) pole_pairs = 7;
    return omega_e * 60.0f / (hostsim::kTwoPi * static_cast<float>(pole_pairs));
}

float platform_get_dc_link_voltage(void) {
    return hostsim::GetSimContext().vdc_v;
}

float platform_phase_voltage_u(void) {
    const auto& c = hostsim::GetSimContext();
    return c.duty_u * c.vdc_v / 100.0f;
}

float platform_phase_voltage_v(void) {
    const auto& c = hostsim::GetSimContext();
    return c.duty_v * c.vdc_v / 100.0f;
}

float platform_phase_voltage_w(void) {
    const auto& c = hostsim::GetSimContext();
    return c.duty_w * c.vdc_v / 100.0f;
}

float platform_get_throttle_a(void) {
    return hostsim::GetSimContext().throttle_a;
}

float platform_get_throttle_b(void) {
    return hostsim::GetSimContext().throttle_b;
}

bool platform_get_throttle_valid(void) {
    const float a = platform_get_throttle_a();
    const float b = platform_get_throttle_b();
    if (a < 0.0f || a > 1.0f || b < 0.0f || b > 1.0f) return false;
    return std::fabs(a - b) < 0.1f;
}

float platform_get_motor_temperature(void) { return 25.0f; }
float platform_get_inverter_temperature(uint8_t channel) {
    (void)channel;
    return 25.0f;
}

bool platform_digital_read(uint8_t pin) {
    (void)pin;
    return false;
}

void platform_digital_write(uint8_t pin, bool value) {
    (void)pin;
    (void)value;
}

bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                       const uint8_t* data, uint8_t dlc) {
    (void)bus;
    (void)id;
    (void)ext;
    (void)data;
    (void)dlc;
    return false;
}

int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out) {
    (void)bus;
    (void)id;
    (void)data;
    (void)seq_out;
    return -1;
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
    hostsim::GlobalTelemetryPublisher().LogF32(key, value);
    static bool stderr_env_checked = false;
    static bool stderr_enabled = false;
    if (!stderr_env_checked) {
        stderr_env_checked = true;
#if defined(_MSC_VER)
        char* env_value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env_value, &len, "HOSTSIM_TELEM_STDERR") == 0 && env_value != nullptr) {
            stderr_enabled = true;
            free(env_value);
        }
#else
        stderr_enabled = std::getenv("HOSTSIM_TELEM_STDERR") != nullptr;
#endif
    }
    if (stderr_enabled) {
        std::fprintf(stderr, "telemetry %s=%g\n", key, static_cast<double>(value));
    }
}

uint32_t platform_millis(void) {
    return static_cast<uint32_t>(hostsim::GetSimContext().time_us / 1000ULL);
}

uint32_t platform_micros(void) {
    return static_cast<uint32_t>(hostsim::GetSimContext().time_us);
}

} // extern "C"
