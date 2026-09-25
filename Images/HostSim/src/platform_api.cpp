#include "platform_api.h"

#include "can_bridge.h"
#include "current_observer.h"
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
    if (g_motor) {
        return g_motor->Params().pole_pairs > 0 ? g_motor->Params().pole_pairs
                                                : 1;
    }
    /* Non-ODE backends: IPlant exposes no params, so SimRuntime stashes the
     * scenario's pole_pairs in the SimContext at domain init (InitDomains).
     * Clamped to >= 1: a division by zero here would poison the encoder. */
    const int pp = GetSimContext().pole_pairs;
    return pp > 0 ? pp : 1;
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
 * Mirrors the Gen6FW PhaseCurrentADC signal chain (constants in RteParams.h),
 * including the inverted sensor wiring documented for the hardware and
 * modeled the same way in HostSIL (sil_phase_current_adc.cpp):
 *   sig_counts = ref_counts - i_measured * counts_per_amp, clamped to the ADC
 *   i_measured = i_true * gain_error + bias + gaussian noise
 *   current a graph recovers = (sig - ref) * lsb / (divider * sensitivity)
 *     minus the calibrated zero offset, i.e. -i_measured + bias
 *
 * Graphs fix the sign with their InvertPolarity parameter (or an explicit
 * negation, e.g. Custom.PhaseCurrentsBurst) exactly as on hardware.
 *
 * Conversions are latch-based like the STM32 injected channels: the runtime
 * calls SimAdcTriggerConversion() at each adc_isr tick; reads after that see a
 * coherent sample set. Direct reads without a trigger convert on demand once
 * per plant step (tracked via SimContext::plant_step_seq).
 *
 * The startup zero-offset calibration is not simulated separately: the
 * "calibrated" offset reported by platform_adc_get_offset_*() is what a
 * Gen6 calibration at standstill would measure, i.e. -(injected bias).
 * -------------------------------------------------------------------------- */
SimAdcConfig g_adc_cfg{};
bool g_adc_valid = false;
uint64_t g_adc_plant_seq = ~0ull;
uint32_t g_adc_u_sig = 0;
uint32_t g_adc_v_sig = 0;
uint32_t g_adc_u_ref = 0;
uint32_t g_adc_v_ref = 0;
uint32_t g_adc_burst_time_us = 0;
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
    /* Inverted transducer wiring, as on the Gen6 hardware: sig counts
     * decrease with positive phase current. */
    long counts = std::lround(static_cast<float>(AdcRefCounts()) - i_meas * counts_per_amp);
    if (counts < 0) counts = 0;                       /* saturate at the rails */
    if (counts > static_cast<long>(max_counts)) counts = max_counts;
    return static_cast<uint32_t>(counts);
}

/* Gen6 PhaseCurrentADC::countsToCurrent: differential counts to amps via the
 * ADC lsb and the transducer chain (divider * sensitivity). */
float AdcCountsToCurrent(uint32_t sig, uint32_t ref) {
    const float vref = g_adc_cfg.vref_v > 1e-6f ? g_adc_cfg.vref_v : 1e-6f;
    const float lsb = vref / static_cast<float>(AdcMaxCounts());
    const float denom = g_adc_cfg.divider * g_adc_cfg.sensitivity_v_per_a;
    const float scale = denom > 1e-12f ? lsb / denom : 0.0f;
    return (static_cast<float>(sig) - static_cast<float>(ref)) * scale;
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
    g_adc_burst_time_us = static_cast<uint32_t>(GetSimContext().time_us);
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

/* --------------------------------------------------------------------------
 * Encoder model (Gen6 EncoderADC semantics; same rendering as HostSIL's
 * sil_encoder_adc.cpp).
 *
 * The analog sin/cos encoder measures the MECHANICAL rotor angle: one
 * sinusoidal cycle per mechanical revolution, captured as 16-bit ADC counts.
 * The plant integrates the electrical angle, so the mechanical angle is
 * theta_e / pole_pairs. Counts are rendered as center 32768, amplitude 30000
 * (inside the driver's 427..65388 hard caps), rounded and rail-clamped.
 * -------------------------------------------------------------------------- */
constexpr uint32_t kEncoderFullScaleCounts = 65535u;
constexpr float kEncoderCenterCounts = 32768.0f;
constexpr float kEncoderAmplitudeCounts = 30000.0f;

float PlantMechanicalDeg() {
    float elec_deg = 0.0f;
    if (g_plant) {
        elec_deg = g_plant->ThetaElectricalDeg();
    } else if (g_motor) {
        elec_deg = g_motor->ThetaElectricalDeg();
    } else {
        return 0.0f;
    }
    const float pp = static_cast<float>(PlantPolePairs());
    float mech_deg = elec_deg / pp;
    mech_deg = std::fmod(mech_deg, 360.0f);
    if (mech_deg < 0.0f) mech_deg += 360.0f;
    return mech_deg;
}

uint32_t EncoderCounts(float value) {
    long counts = std::lround(value);
    if (counts < 0) counts = 0;
    if (counts > static_cast<long>(kEncoderFullScaleCounts)) {
        counts = static_cast<long>(kEncoderFullScaleCounts);
    }
    return static_cast<uint32_t>(counts);
}

/* Gen6 TIM1 hardware configuration (Src/tim.c): 275 MHz timer clock,
 * center-aligned, ARR 27500. */
constexpr uint32_t kPwmTimerArr = 27500u;

/* Gen6 platform_schedule_adaptive_sample: deadtime + switching settling +
 * ADC burst = 6 us at the 275 MHz timer clock. */
constexpr uint32_t kSampleMinGapTicks = 1650u;

/* Store for the platform domain-dt pair (Gen6 storage semantics; the
 * scheduler sets it before each generated domain step, see sim_runtime.cpp). */
float g_current_domain_dt = 0.0f;

/* --------------------------------------------------------------------------
 * Current-observer platform state.
 *
 * Mirrors the Gen6 platform_api.cpp observer block: a plain use_observer flag
 * (set by generated code / a shell command on hardware; nothing inside the
 * base image consumes it) plus the calibration snapshot used by
 * platform_observer_init_from_calibration().
 *
 * The sim has no FRAM-backed MotorCalibration; the scenario "motor"
 * parameters are the calibration source.  SimRuntime seeds them through
 * SimObserverConfigure() at domain init — before generated constructors run —
 * applying and resetting the observer once so it is plausible even when the
 * graph never instantiates hw.current_observer.  init_from_calibration()
 * re-applies the snapshot and resets, matching the Gen6 generated-init path.
 * -------------------------------------------------------------------------- */
bool g_use_observer = false;

struct ObserverCal {
    float r_ohm = 0.0f;
    float l_henry = 0.0f;
    float flux_wb = 0.0f;
    float pole_pairs = 0.0f;
    bool valid = false;
};

ObserverCal g_observer_cal{};

void ApplyObserverCal(const ObserverCal& cal) {
    GlobalCurrentObserver().setMotorParameters(cal.r_ohm, cal.l_henry,
                                               cal.flux_wb, cal.pole_pairs);
}
} // namespace

void SimObserverConfigure(float r_ohm, float l_henry, float flux_wb,
                          float pole_pairs) {
    ObserverCal cal{r_ohm, l_henry, flux_wb, pole_pairs, true};
    g_observer_cal = cal;
    ApplyObserverCal(cal);
    GlobalCurrentObserver().reset();
}

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

uint32_t platform_pwm_get_arr(void) { return hostsim::kPwmTimerArr; }

uint32_t platform_schedule_adaptive_sample(float duty_u, float duty_v,
                                           float duty_w, uint32_t arr) {
    /* Port of Gen6FW PWM_FindSafeSamplePoint (Src/Inverter/Drivers/PWM/
     * pwm.cpp): center-aligned PWM with low-side shunts; the quiet windows
     * are the all-low span 2*(arr - max_ccr) and the all-high span
     * 2*min_ccr. Take the larger; below the minimum gap the firmware falls
     * back to the legacy bottom trigger and reports 0. */
    const auto clamp_duty = [](float d) {
        return std::max(0.0f, std::min(100.0f, d));
    };
    const uint32_t ccr_u =
        static_cast<uint32_t>(clamp_duty(duty_u) * static_cast<float>(arr) /
                              100.0f);
    const uint32_t ccr_v =
        static_cast<uint32_t>(clamp_duty(duty_v) * static_cast<float>(arr) /
                              100.0f);
    const uint32_t ccr_w =
        static_cast<uint32_t>(clamp_duty(duty_w) * static_cast<float>(arr) /
                              100.0f);

    const uint32_t min_ccr = std::min({ccr_u, ccr_v, ccr_w});
    const uint32_t max_ccr = std::max({ccr_u, ccr_v, ccr_w});
    const uint32_t gap_all_high = 2u * min_ccr;
    const uint32_t gap_all_low = 2u * (arr - max_ccr);
    const uint32_t best_gap =
        gap_all_low >= gap_all_high ? gap_all_low : gap_all_high;

    if (best_gap < hostsim::kSampleMinGapTicks) return 0u;
    return best_gap;
}

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    /* Gen6 PhaseCurrentADC::sample(): sensor-model values (inverted wiring,
     * latched at the last conversion trigger), zero-offset removed, W
     * reconstructed from U+V — not a direct plant read. */
    if (!hostsim::CurrentPlantState()) return false;
    hostsim::AdcEnsureFresh();
    const float iu = hostsim::AdcCountsToCurrent(hostsim::g_adc_u_sig,
                                                 hostsim::g_adc_u_ref) -
                     platform_adc_get_offset_u_a();
    const float iv = hostsim::AdcCountsToCurrent(hostsim::g_adc_v_sig,
                                                 hostsim::g_adc_v_ref) -
                     platform_adc_get_offset_v_a();
    if (iu_a) *iu_a = iu;
    if (iv_a) *iv_a = iv;
    if (iw_a) *iw_a = -(iu + iv);
    return true;
}

/* --------------------------------------------------------------------------
 * Current observer — thin wrappers over the ported CurrentObserver
 * (src/current_observer.cpp), same call surfaces as the Gen6 platform_api.cpp
 * observer block.  Gating on use_observer is deliberately NOT done here:
 * Gen6 keeps the observer free-running and lets the control path (native
 * FOC or a graph gate node) select feedback. */

void platform_set_use_observer(bool enabled) {
    hostsim::g_use_observer = enabled;
}

bool platform_get_use_observer(void) { return hostsim::g_use_observer; }

void platform_get_observer_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (!iu_a || !iv_a || !iw_a) return;
    hostsim::GlobalCurrentObserver().getPhaseCurrents(*iu_a, *iv_a, *iw_a);
}

void platform_observer_predict(float valpha_v, float vbeta_v,
                               float theta_elec_rad, float dt_s) {
    hostsim::GlobalCurrentObserver().predict(valpha_v, vbeta_v,
                                             theta_elec_rad, dt_s);
}

void platform_observer_set_motor_params(float r_ohm, float l_henry,
                                        float flux_linkage_wb,
                                        float pole_pairs) {
    hostsim::GlobalCurrentObserver().setMotorParameters(r_ohm, l_henry,
                                                        flux_linkage_wb,
                                                        pole_pairs);
}

void platform_observer_init_from_calibration(void) {
    /* Gen6 re-reads MotorCalibration here and resets.  The sim's calibration
     * is the scenario motor block, seeded via SimObserverConfigure() at
     * domain init; without a seed (no scenario yet, e.g. a bare unit harness)
     * the observer keeps its Gen6 default parameters. */
    if (hostsim::g_observer_cal.valid) {
        hostsim::ApplyObserverCal(hostsim::g_observer_cal);
    }
    hostsim::GlobalCurrentObserver().reset();
}

void platform_observer_correct(float iu_meas_a, float iv_meas_a,
                               float diudt_a_per_s, float divdt_a_per_s,
                               uint32_t t_us) {
    hostsim::GlobalCurrentObserver().correct(iu_meas_a, iv_meas_a,
                                             diudt_a_per_s, divdt_a_per_s,
                                             t_us);
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
/* Gen6 lastOffsetU/V: what startup calibration measures at standstill — the
 * recovered (inverted) amps of the injected bias, i.e. -(bias). */
float platform_adc_get_offset_u_a(void) { return -hostsim::g_adc_cfg.offset_u_a; }
float platform_adc_get_offset_v_a(void) { return -hostsim::g_adc_cfg.offset_v_a; }

bool platform_adc_get_burst_sample(float* iu0_a, float* iv0_a,
                                   float* iu1_a, float* iv1_a,
                                   uint32_t* time_us) {
    if (!iu0_a || !iv0_a || !iu1_a || !iv1_a) return false;
    if (!hostsim::CurrentPlantState()) return false;
    hostsim::AdcEnsureFresh();
    const float iu = hostsim::AdcCountsToCurrent(hostsim::g_adc_u_sig,
                                                 hostsim::g_adc_u_ref) -
                     platform_adc_get_offset_u_a();
    const float iv = hostsim::AdcCountsToCurrent(hostsim::g_adc_v_sig,
                                                 hostsim::g_adc_v_ref) -
                     platform_adc_get_offset_v_a();
    /* Gen6 samples ranks 1/2 and 3/4 back-to-back (~0.46 us apart). The sim's
     * conversion latch is zero-order-held across the burst, so both points
     * read the same latched sample set. */
    *iu0_a = iu;
    *iu1_a = iu;
    *iv0_a = iv;
    *iv1_a = iv;
    if (time_us) *time_us = hostsim::g_adc_burst_time_us;
    return true;
}

bool platform_get_encoder_angle(float* angle_deg) {
    if (!hostsim::g_plant && !hostsim::g_motor) return false;
    if (angle_deg) *angle_deg = hostsim::PlantMechanicalDeg();
    auto& ctx = hostsim::GetSimContext();
    const bool had = ctx.encoder_sample_new;
    ctx.encoder_sample_new = false;
    return had;
}

float platform_get_encoder_angle_latest(void) {
    return hostsim::PlantMechanicalDeg();
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

float platform_get_rpm_mech(void) { return platform_get_motor_rpm(); }

float platform_get_rpm_elec(void) {
    /* Gen6: rpmMech * pole_pairs * MotorCalibration.encoder_sign. The
     * simulated encoder counts in the positive rotation direction, so the
     * sign is +1. */
    return platform_get_rpm_mech() *
           static_cast<float>(hostsim::PlantPolePairs());
}

float platform_get_control_rpm_mech(void) { return platform_get_rpm_mech(); }
float platform_get_control_rpm_elec(void) { return platform_get_rpm_elec(); }

uint32_t platform_get_encoder_raw_sin(void) {
    const float theta_m_rad =
        hostsim::PlantMechanicalDeg() * (hostsim::kTwoPi / 360.0f);
    return hostsim::EncoderCounts(hostsim::kEncoderCenterCounts +
                                  hostsim::kEncoderAmplitudeCounts *
                                      std::sin(theta_m_rad));
}

uint32_t platform_get_encoder_raw_cos(void) {
    const float theta_m_rad =
        hostsim::PlantMechanicalDeg() * (hostsim::kTwoPi / 360.0f);
    return hostsim::EncoderCounts(hostsim::kEncoderCenterCounts +
                                  hostsim::kEncoderAmplitudeCounts *
                                      std::cos(theta_m_rad));
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
    /* Mirror every send onto the multi-instance CAN bridge (no-op until a
     * --can-bridge-* flag configured it). Deliberately ahead of the bus
     * validity check: bus 0 is not a local bus (Gen6 numbering is 1-based)
     * but exists on the bridge as the selftest/diagnostic channel; it never
     * enters the local store below. */
    hostsim::GlobalCanBridge().Publish(bus, id, ext, data, dlc);
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

void platform_set_current_domain_dt(float dt_s) {
    hostsim::g_current_domain_dt = dt_s;
}

float platform_get_current_domain_dt(void) {
    return hostsim::g_current_domain_dt;
}

} // extern "C"
