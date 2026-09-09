#include "platform_api.h"

#include "motor_model.h"
#include "pwm_scope.h"
#include "sim_context.h"
#include "telemetry_publisher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(_MSC_VER)
#include <stdlib.h>
#endif
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "plant/plant_backend.h"
#include "plant/ode_plant.h"

namespace hostsim {

IPlant* g_plant = nullptr;
/* OdePlant backing model (set exclusively by SimRuntime_RegisterPlant when
 * the active plant is an OdePlant). Not a parallel plant path: every plant
 * read goes through g_plant first; g_motor only supplies data the IPlant
 * interface does not expose (pole pairs, per-phase terminal voltages recorded
 * by MotorModel). */
MotorModel* g_motor = nullptr;

namespace {

const MotorState* CurrentPlantState() {
    if (g_plant) return &g_plant->State();
    if (g_motor) return &g_motor->State();
    return nullptr;
}

int PlantPolePairs() {
    return g_motor ? g_motor->Params().pole_pairs : 7;
}

std::mutex g_cfg_mu;
std::unordered_map<std::string, float> g_config;
std::string g_config_file;

void PersistConfigLocked() {
    if (g_config_file.empty()) return;
    std::ofstream out(g_config_file, std::ios::out | std::ios::trunc);
    if (!out) return;
    std::vector<std::string> keys;
    keys.reserve(g_config.size());
    for (const auto& kv : g_config) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (const auto& k : keys) {
        out << k << '=' << g_config[k] << '\n';
    }
    out.flush();
}

std::recursive_mutex g_critical_mu;

/* --------------------------------------------------------------------------
 * Phase-current ADC sensor model.
 *
 * Mirrors the Gen6FW PhaseCurrentADC signal chain (constants in RteParams.h):
 *   sig_counts = ref_counts + i_measured * counts_per_amp, clamped to the ADC
 *   i_measured = i_true * gain_error + bias + gaussian noise
 *   current a graph recovers = (sig - ref) * lsb / (divider * sensitivity)
 *
 * Conversions are latch-based like the STM32 injected channels: the runtime
 * calls SimAdcTriggerConversion() at each adc_isr tick; reads after that see a
 * coherent sample set. Direct reads without a trigger convert on demand once
 * per plant step (tracked via SimContext::plant_step_seq).
 * -------------------------------------------------------------------------- */
SimAdcConfig g_adc_cfg{};
bool g_adc_valid = false;
uint64_t g_adc_plant_seq = ~0ull;
uint32_t g_adc_u_sig = 0;
uint32_t g_adc_v_sig = 0;
uint32_t g_adc_u_ref = 0;
uint32_t g_adc_v_ref = 0;
std::mt19937 g_adc_rng{0xC0FFEEu};
std::normal_distribution<float> g_adc_noise{0.0f, 1.0f};

unsigned AdcMaxCounts() { return (1u << g_adc_cfg.bits) - 1u; }

uint32_t AdcRefCounts() {
    const unsigned max_counts = AdcMaxCounts();
    const float vref = g_adc_cfg.vref_v > 1e-6f ? g_adc_cfg.vref_v : 1e-6f;
    long counts = std::lround(g_adc_cfg.ref_volts / vref * static_cast<float>(max_counts));
    if (counts < 0) counts = 0;
    if (counts > static_cast<long>(max_counts)) counts = max_counts;
    return static_cast<uint32_t>(counts);
}

uint32_t AdcCurrentToCounts(float i_true_a, float bias_a) {
    const unsigned max_counts = AdcMaxCounts();
    const float vref = g_adc_cfg.vref_v > 1e-6f ? g_adc_cfg.vref_v : 1e-6f;
    const float counts_per_amp = g_adc_cfg.divider * g_adc_cfg.sensitivity_v_per_a *
                                 static_cast<float>(max_counts) / vref;
    float i_meas = i_true_a * g_adc_cfg.gain_error + bias_a;
    if (g_adc_cfg.noise_std_a > 0.0f) {
        i_meas += g_adc_noise(g_adc_rng) * g_adc_cfg.noise_std_a;
    }
    long counts = std::lround(static_cast<float>(AdcRefCounts()) + i_meas * counts_per_amp);
    if (counts < 0) counts = 0;                       /* saturate at the rails */
    if (counts > static_cast<long>(max_counts)) counts = max_counts;
    return static_cast<uint32_t>(counts);
}

void AdcConvert() {
    float iu = 0.0f;
    float iv = 0.0f;
    if (const MotorState* st = CurrentPlantState()) {
        iu = st->ia_a;
        iv = st->ib_a;
    }
    g_adc_u_ref = AdcRefCounts();
    g_adc_v_ref = AdcRefCounts();
    g_adc_u_sig = AdcCurrentToCounts(iu, g_adc_cfg.offset_u_a);
    g_adc_v_sig = AdcCurrentToCounts(iv, g_adc_cfg.offset_v_a);
    g_adc_plant_seq = GetSimContext().plant_step_seq;
    g_adc_valid = true;
}

void AdcEnsureFresh() {
    if (!g_adc_valid || g_adc_plant_seq != GetSimContext().plant_step_seq) {
        AdcConvert();
    }
}

/* --------------------------------------------------------------------------
 * CAN: latest-frame loopback store, keyed by (bus, id), like the Gen6
 * rxLatest path. Sent frames are readable via platform_can_rx when loopback
 * is enabled; scenario frames arrive via SimCanInjectFrame regardless.
 * -------------------------------------------------------------------------- */
struct CanRxFrame {
    uint32_t id = 0;
    bool ext = false;
    uint8_t dlc = 0;
    uint8_t data[8] = {0};
    uint32_t seq = 0;
};

std::mutex g_can_mu;
std::unordered_map<uint64_t, CanRxFrame> g_can_rx;
std::unordered_map<uint64_t, uint32_t> g_can_seq;
bool g_can_loopback = true;

uint64_t CanKey(uint8_t bus, uint32_t id) {
    return (static_cast<uint64_t>(bus) << 32) | id;
}

void CanStoreUnlocked(uint8_t bus, uint32_t id, bool ext, const uint8_t* data,
                      uint8_t dlc) {
    CanRxFrame f{};
    f.id = id;
    f.ext = ext;
    f.dlc = dlc > 8 ? 8 : dlc;
    for (uint8_t i = 0; i < f.dlc; ++i) f.data[i] = data ? data[i] : 0;
    f.seq = ++g_can_seq[CanKey(bus, id)];
    g_can_rx[CanKey(bus, id)] = f;
}

void CanStore(uint8_t bus, uint32_t id, bool ext, const uint8_t* data,
              uint8_t dlc) {
    std::lock_guard<std::mutex> lock(g_can_mu);
    CanStoreUnlocked(bus, id, ext, data, dlc);
}

/* Digital IO: writes latch, reads see the latched level (1..N pins). */
std::unordered_map<uint8_t, bool> g_dio;

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

void SimRuntime_RegisterPlant(IPlant* plant) {
    g_plant = plant;
    auto* ode = dynamic_cast<OdePlant*>(plant);
    g_motor = ode ? &ode->Model() : nullptr;
}

void SimAdcConfigure(const SimAdcConfig& cfg) {
    g_adc_cfg = cfg;
    g_adc_valid = false;
}

void SimAdcTriggerConversion() { AdcConvert(); }

void SimCanSetLoopback(bool enabled) {
    std::lock_guard<std::mutex> lock(g_can_mu);
    g_can_loopback = enabled;
    g_can_rx.clear();
    g_can_seq.clear();
}

void SimCanInject(uint8_t bus, uint32_t id, bool ext,
                  const uint8_t* data, uint8_t dlc) {
    CanStore(bus, id, ext, data, dlc);
}

void SimConfigSetBackingFile(const char* path) {
    std::lock_guard<std::mutex> lock(g_cfg_mu);
    g_config_file = path ? path : "";
    if (g_config_file.empty()) return;
    /* Merge any persisted keys; "key=value" lines, '#' comments. */
    std::ifstream in(g_config_file);
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        char* end = nullptr;
        const float v = std::strtof(line.c_str() + eq + 1, &end);
        if (end == line.c_str() + eq + 1 || key.empty()) continue;
        g_config[key] = v;
    }
}

void SimConfigPersist() {
    std::lock_guard<std::mutex> lock(g_cfg_mu);
    PersistConfigLocked();
}

} // namespace hostsim

extern "C" {

void platform_pwm_set(float du, float dv, float dw) {
    auto& c = hostsim::GetSimContext();
    c.duty_u = du;
    c.duty_v = dv;
    c.duty_w = dw;
    c.pwm_written = true;
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
    const hostsim::MotorState* st = hostsim::CurrentPlantState();
    if (!st) return false;
    if (iu_a) *iu_a = st->ia_a;
    if (iv_a) *iv_a = st->ib_a;
    if (iw_a) *iw_a = st->ic_a;
    return true;
}

uint32_t platform_adc_get_injected_u_sig(void) {
    hostsim::AdcEnsureFresh();
    return hostsim::g_adc_u_sig;
}
uint32_t platform_adc_get_injected_v_sig(void) {
    hostsim::AdcEnsureFresh();
    return hostsim::g_adc_v_sig;
}
uint32_t platform_adc_get_injected_u_ref(void) {
    hostsim::AdcEnsureFresh();
    return hostsim::g_adc_u_ref;
}
uint32_t platform_adc_get_injected_v_ref(void) {
    hostsim::AdcEnsureFresh();
    return hostsim::g_adc_v_ref;
}
float platform_adc_get_offset_u_a(void) { return hostsim::g_adc_cfg.offset_u_a; }
float platform_adc_get_offset_v_a(void) { return hostsim::g_adc_cfg.offset_v_a; }

bool platform_get_encoder_angle(float* angle_deg) {
    if (hostsim::g_plant && angle_deg) {
        *angle_deg = hostsim::g_plant->ThetaElectricalDeg();
    } else if (hostsim::g_motor && angle_deg) {
        *angle_deg = hostsim::g_motor->ThetaElectricalDeg();
    } else if (!hostsim::g_plant && !hostsim::g_motor) {
        return false;
    }
    auto& ctx = hostsim::GetSimContext();
    const bool had = ctx.encoder_sample_new;
    ctx.encoder_sample_new = false;
    return had;
}

float platform_get_encoder_angle_latest(void) {
    if (hostsim::g_plant) return hostsim::g_plant->ThetaElectricalDeg();
    if (hostsim::g_motor) return hostsim::g_motor->ThetaElectricalDeg();
    return 0.0f;
}

float platform_get_motor_rpm(void) {
    float omega_e = 0.0f;
    if (hostsim::g_plant) {
        omega_e = hostsim::g_plant->OmegaElectricalRadPerSec();
    } else if (hostsim::g_motor) {
        omega_e = hostsim::g_motor->OmegaElectricalRadPerSec();
    } else {
        return 0.0f;
    }
    return omega_e * 60.0f /
           (hostsim::kTwoPi * static_cast<float>(hostsim::PlantPolePairs()));
}

float platform_get_dc_link_voltage(void) {
    return hostsim::GetSimContext().vdc_v;
}

/* Phase voltage readback: what the plant actually applied last step, not the
 * duty request. For OdePlant the MotorModel records its clamped duty*vdc
 * terminals; other backends fall back to applied duty * plant DC link. */
float platform_phase_voltage_u(void) {
    if (hostsim::g_motor) return hostsim::g_motor->State().va_v;
    const auto& c = hostsim::GetSimContext();
    return c.duty_applied_u * c.plant_vdc_v / 100.0f;
}

float platform_phase_voltage_v(void) {
    if (hostsim::g_motor) return hostsim::g_motor->State().vb_v;
    const auto& c = hostsim::GetSimContext();
    return c.duty_applied_v * c.plant_vdc_v / 100.0f;
}

float platform_phase_voltage_w(void) {
    if (hostsim::g_motor) return hostsim::g_motor->State().vc_v;
    const auto& c = hostsim::GetSimContext();
    return c.duty_applied_w * c.plant_vdc_v / 100.0f;
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

float platform_get_motor_temperature(void) {
    return hostsim::GetSimContext().motor_temp_c;
}
float platform_get_inverter_temperature(uint8_t channel) {
    (void)channel;
    return hostsim::GetSimContext().inverter_temp_c;
}

bool platform_digital_read(uint8_t pin) {
    auto it = hostsim::g_dio.find(pin);
    return it != hostsim::g_dio.end() && it->second;
}

void platform_digital_write(uint8_t pin, bool value) {
    hostsim::g_dio[pin] = value;
}

bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                       const uint8_t* data, uint8_t dlc) {
    if (bus < 1 || bus > 2) return false;
    {
        std::lock_guard<std::mutex> lock(hostsim::g_can_mu);
        if (hostsim::g_can_loopback) {
            hostsim::CanStoreUnlocked(bus, id, ext, data, dlc);
        }
    }
    /* The frame always "goes on the wire" from the sender's point of view;
     * loopback only controls whether this node reads its own frames back. */
    return true;
}

int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out) {
    if (bus < 1 || bus > 2) {
        if (seq_out) *seq_out = 0;
        return -1;
    }
    std::lock_guard<std::mutex> lock(hostsim::g_can_mu);
    const auto it = hostsim::g_can_rx.find(hostsim::CanKey(bus, id));
    if (it == hostsim::g_can_rx.end()) {
        if (seq_out) *seq_out = 0;
        return -1;
    }
    const hostsim::CanRxFrame& f = it->second;
    if (data) {
        for (uint8_t i = 0; i < f.dlc; ++i) data[i] = f.data[i];
    }
    if (seq_out) *seq_out = f.seq;
    return f.dlc;
}

void platform_sample_application_sensors(void) { hostsim::SimAdcTriggerConversion(); }

void platform_raise_fault(uint32_t source, uint8_t reason) {
    (void)source;
    (void)reason;
    hostsim::GetSimContext().critical_fault = true;
}

bool platform_has_critical_fault(void) {
    return hostsim::GetSimContext().critical_fault;
}

void platform_critical_enter(void) { hostsim::g_critical_mu.lock(); }
void platform_critical_exit(void) { hostsim::g_critical_mu.unlock(); }

float platform_config_load(const char* key, float default_value) {
    if (!key) return default_value;
    std::lock_guard<std::mutex> lock(hostsim::g_cfg_mu);
    auto it = hostsim::g_config.find(key);
    if (it != hostsim::g_config.end()) return it->second;
    hostsim::g_config[key] = default_value;
    hostsim::PersistConfigLocked();
    return default_value;
}

void platform_config_set(const char* key, float value) {
    if (!key) return;
    std::lock_guard<std::mutex> lock(hostsim::g_cfg_mu);
    hostsim::g_config[key] = value;
    hostsim::PersistConfigLocked();
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
