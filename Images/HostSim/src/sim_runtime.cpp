#include "sim_runtime.h"

#include "AppState.h"
#include "can_bridge.h"
#include "pwm_scope.h"
#include "sim_context.h"
#include "telemetry_publisher.h"
#include "plant/plant_backend.h"
#include "plant/ode_plant.h"
#include "plant/ngspice_plant.h"
#include "platform_api.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "realtime_platform.h"

/* Graph Var registries exist only after RTECodeEmitter has generated the
 * domain sources. The base image (and HostSIL, which reuses this file only
 * in spirit) compiles without them; guard the same way AppState.h does. */
#if defined(__has_include)
#if __has_include("../generated/domain_tim_isr_generated.h") && \
    __has_include("../generated/domain_app_loop_generated.h") && \
    __has_include("../generated/domain_adc_isr_generated.h") && \
    __has_include("../generated/domain_vsense_generated.h")
#include "../generated/domain_tim_isr_generated.h"
#include "../generated/domain_app_loop_generated.h"
#include "../generated/domain_adc_isr_generated.h"
#include "../generated/domain_vsense_generated.h"
#define HOSTSIM_HAS_GENERATED_DOMAINS 1
#endif
#endif
#ifndef HOSTSIM_HAS_GENERATED_DOMAINS
#define HOSTSIM_HAS_GENERATED_DOMAINS 0
#endif

namespace hostsim {

SimRuntime::SimRuntime() : plant_(std::make_unique<OdePlant>()) {}
SimRuntime::~SimRuntime() = default;

namespace {

SimRuntime g_runtime;

std::string ExtractString(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return {};
    const size_t colon = blob.find(':', pos);
    const size_t q1 = blob.find('"', colon);
    const size_t q2 = blob.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) return {};
    return blob.substr(q1 + 1, q2 - q1 - 1);
}

bool ExtractNumber(const std::string& blob, const std::string& key, float* out) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return false;
    const size_t colon = blob.find(':', pos);
    if (colon == std::string::npos) return false;
    const char* start = blob.c_str() + colon + 1;
    char* end = nullptr;
    const float v = std::strtof(start, &end);
    if (end == start) return false;
    if (out) *out = v;
    return true;
}

/* Handles both JSON booleans and 0/1 numbers. Deliberately parses the token
 * right after the colon so unquoted true/false don't scoop the next key. */
bool ExtractBool(const std::string& blob, const std::string& key, bool* out) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return false;
    const size_t colon = blob.find(':', pos);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < blob.size() && std::isspace(static_cast<unsigned char>(blob[i]))) ++i;
    if (blob.compare(i, 4, "true") == 0) {
        if (out) *out = true;
        return true;
    }
    if (blob.compare(i, 5, "false") == 0) {
        if (out) *out = false;
        return true;
    }
    char* end = nullptr;
    const float v = std::strtof(blob.c_str() + i, &end);
    if (end == blob.c_str() + i) return false;
    if (out) *out = (v != 0.0f);
    return true;
}

/* Unsigned integer with base auto-detect (supports "0x" hex CAN ids). */
bool ExtractUint(const std::string& blob, const std::string& key, uint32_t* out) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return false;
    const size_t colon = blob.find(':', pos);
    if (colon == std::string::npos) return false;
    const char* start = blob.c_str() + colon + 1;
    char* end = nullptr;
    const unsigned long v = std::strtoul(start, &end, 0);
    if (end == start) return false;
    if (out) *out = static_cast<uint32_t>(v);
    return true;
}

std::string ExtractObject(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return {};
    const size_t brace = blob.find('{', pos);
    if (brace == std::string::npos) return {};
    int depth = 0;
    for (size_t i = brace; i < blob.size(); ++i) {
        if (blob[i] == '{') ++depth;
        if (blob[i] == '}') {
            --depth;
            if (depth == 0) return blob.substr(brace, i - brace + 1);
        }
    }
    return {};
}

/* Bracket-matched "key": [ ... ] extraction, inner content only. */
std::string ExtractArray(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return {};
    const size_t open = blob.find('[', pos);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (size_t i = open; i < blob.size(); ++i) {
        if (blob[i] == '[') ++depth;
        if (blob[i] == ']') {
            --depth;
            if (depth == 0) return blob.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

/* Split an array body into its top-level {...} object bodies. */
std::vector<std::string> SplitArrayObjects(const std::string& array_body) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < array_body.size(); ++i) {
        if (array_body[i] == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (array_body[i] == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(array_body.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

/* Hex string payload → byte buffer, e.g. "DEADBEEF" -> {0xDE,0xAD,0xBE,0xEF}.
 * Non-hex pairs are skipped; result capped at max_bytes. */
uint8_t ParseHexPayload(const std::string& hex, uint8_t* out, uint8_t max_bytes) {
    if (!out || max_bytes == 0) return 0;
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) {
        if (std::isxdigit(static_cast<unsigned char>(c))) clean.push_back(c);
    }
    uint8_t n = 0;
    for (size_t i = 0; i + 1 < clean.size() && n < max_bytes; i += 2) {
        out[n++] = static_cast<uint8_t>(std::strtoul(clean.substr(i, 2).c_str(), nullptr, 16));
    }
    return n;
}

/* Enumerate "key": number pairs in a flat object blob (for the scenario
 * "vars" graph-var seed map). String values would otherwise alias the next
 * pair's colon (a value in quotes is not a number): skip them instead. */
void EnumerateKv(const std::string& blob,
                 std::vector<std::pair<std::string, float>>* out) {
    if (!out) return;
    size_t i = 0;
    while (i < blob.size()) {
        const size_t q1 = blob.find('"', i);
        if (q1 == std::string::npos) break;
        const size_t q2 = blob.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string key = blob.substr(q1 + 1, q2 - q1 - 1);
        const size_t colon = blob.find(':', q2);
        if (colon == std::string::npos) break;
        size_t start = colon + 1;
        while (start < blob.size() &&
               std::isspace(static_cast<unsigned char>(blob[start]))) ++start;
        if (start < blob.size() && blob[start] == '"') {
            /* String value: skip to its closing quote so it cannot alias the
             * next pair's colon. */
            const size_t vend = blob.find('"', start + 1);
            i = (vend != std::string::npos) ? vend + 1 : blob.size();
            continue;
        }
        char* end = nullptr;
        const float v = std::strtof(blob.c_str() + start, &end);
        if (end != blob.c_str() + start) {
            out->emplace_back(key, v);
        }
        i = colon + 1;
    }
}

StimulusType ParseStimulusType(const std::string& blob) {
    const std::string t = ExtractString(blob, "type");
    if (t == "ramp") return StimulusType::Ramp;
    if (t == "step") return StimulusType::Step;
    return StimulusType::Constant;
}

StimulusProfile ParseStimulus(const std::string& blob) {
    StimulusProfile p{};
    p.type = ParseStimulusType(blob);
    float v = 0.0f;
    if (ExtractNumber(blob, "value", &v)) p.value = v;
    if (ExtractNumber(blob, "start", &v)) p.start = v;
    if (ExtractNumber(blob, "end", &v)) p.end = v;
    if (ExtractNumber(blob, "start_s", &v)) p.start_s = v;
    if (ExtractNumber(blob, "end_s", &v)) p.end_s = v;
    if (ExtractNumber(blob, "step_time_s", &v)) p.step_time_s = v;
    if (ExtractNumber(blob, "step_value", &v)) p.step_value = v;
    return p;
}

} // namespace

SimRuntime& GlobalSimRuntime() { return g_runtime; }

void SimRuntime::SetRealtimeFactor(float factor) {
    config_.realtime_factor = factor;
    GlobalTelemetryPublisher().LogF32(
        "sim_speed", factor <= 0.0f ? -1.0f : factor);
}

void SimRuntime::ResetWallClockAnchor() {
    wall_anchor_ = std::chrono::steady_clock::now();
    sim_anchor_s_ = time_s_;
}

float SimRuntime::EffectivePwmTelemHz() const {
    if (!config_.pwm_scope_enabled) {
        return 0.0f;
    }

    float base = config_.pwm_telem_hz;
    if (base <= 0.0f) {
        base = std::clamp(config_.pwm_carrier_hz * 20.0f, 800.0f, 1500.0f);
    }

    constexpr float kCap = 1500.0f;
    if (config_.realtime_factor <= 0.0f) {
        return kCap;
    }

    const float speed = std::max(config_.realtime_factor, 0.05f);
    return std::clamp(base / speed, 400.0f, kCap);
}

void SimRuntime::PublishPwmScopeFrame() {
    PublishPwmScopeTelemetry();
    GlobalTelemetryPublisher().PublishPrefixCycle(static_cast<uint32_t>(TimeMicros()),
                                                  "pwm_");
}

float SimRuntime::EvaluateStimulus(const StimulusProfile& profile) const {
    switch (profile.type) {
    case StimulusType::Constant:
        return profile.value;
    case StimulusType::Ramp:
        if (time_s_ <= profile.start_s) return profile.start;
        if (time_s_ >= profile.end_s) return profile.end;
        if (profile.end_s <= profile.start_s) return profile.end;
        {
            const double t = (time_s_ - static_cast<double>(profile.start_s)) /
                             (static_cast<double>(profile.end_s) - profile.start_s);
            return profile.start + static_cast<float>(t) * (profile.end - profile.start);
        }
    case StimulusType::Step:
        return time_s_ >= static_cast<double>(profile.step_time_s)
                   ? profile.step_value
                   : profile.value;
    }
    return 0.0f;
}

bool SimRuntime::ParseScenario(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "HostSim: cannot open scenario " << path << '\n';
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string blob = ss.str();

    const std::string motor = ExtractObject(blob, "motor");
    const std::string sim = ExtractObject(blob, "simulation");
    float v = 0.0f;
    if (ExtractNumber(motor, "rs_ohm", &v)) config_.motor.rs_ohm = v;
    if (ExtractNumber(motor, "ld_h", &v)) config_.motor.ld_h = v;
    if (ExtractNumber(motor, "lq_h", &v)) config_.motor.lq_h = v;
    if (ExtractNumber(motor, "flux_wb", &v)) config_.motor.flux_wb = v;
    if (ExtractNumber(motor, "pole_pairs", &v)) config_.motor.pole_pairs = static_cast<int>(v);
    if (ExtractNumber(motor, "inertia_kg_m2", &v)) config_.motor.inertia_kg_m2 = v;
    if (ExtractNumber(motor, "friction_nm_per_rad_s", &v)) config_.motor.friction_nm_per_rad_s = v;
    if (ExtractNumber(motor, "vdc_v", &v)) config_.motor.vdc_v = v;
    /* Machine selection: "machine": "induction" swaps the ODE plant to the
     * stationary alpha/beta squirrel-cage model (src/induction_model.h);
     * default/absent stays PMSM. */
    {
        const std::string machine = ExtractString(motor, "machine");
        if (machine == "induction") {
            config_.motor.machine = MachineType::Induction;
        } else if (machine == "pmsm") {
            config_.motor.machine = MachineType::Pmsm;
        } else if (!machine.empty()) {
            std::cerr << "HostSim: unknown motor.machine \"" << machine
                      << "\" (want \"pmsm\" or \"induction\"); keeping pmsm\n";
        }
    }
    if (ExtractNumber(motor, "rr_ohm", &v)) config_.motor.rr_ohm = v;
    if (ExtractNumber(motor, "lm_h", &v)) config_.motor.lm_h = v;
    if (ExtractNumber(motor, "lls_h", &v)) config_.motor.lls_h = v;
    if (ExtractNumber(motor, "llr_h", &v)) config_.motor.llr_h = v;

    if (ExtractNumber(sim, "duration_s", &v)) config_.duration_s = v;
    if (ExtractNumber(sim, "tim_isr_hz", &v)) config_.tim_isr_hz = v;
    if (ExtractNumber(sim, "adc_isr_hz", &v)) config_.adc_isr_hz = v;
    if (ExtractNumber(sim, "app_loop_hz", &v)) config_.app_loop_hz = v;
    if (ExtractNumber(sim, "telem_hz", &v)) config_.telem_hz = v;
    if (ExtractNumber(sim, "realtime_factor", &v)) config_.realtime_factor = v;
    {
        const std::string pwm_scope = ExtractObject(blob, "pwm_scope");
        if (!pwm_scope.empty()) {
            config_.pwm_scope_enabled = true;
            float enabled = 1.0f;
            if (ExtractNumber(pwm_scope, "enabled", &enabled)) {
                config_.pwm_scope_enabled = enabled != 0.0f;
            }
            const std::string enabled_s = ExtractString(pwm_scope, "enabled");
            if (enabled_s == "false" || enabled_s == "0") {
                config_.pwm_scope_enabled = false;
            }
            if (pwm_scope.find("\"enabled\": false") != std::string::npos ||
                pwm_scope.find("\"enabled\":false") != std::string::npos) {
                config_.pwm_scope_enabled = false;
            }
            if (ExtractNumber(pwm_scope, "carrier_hz", &v)) {
                config_.pwm_carrier_hz = v;
            }
            if (ExtractNumber(pwm_scope, "telem_hz", &v)) {
                config_.pwm_telem_hz = v;
            }
        }
        float carrier = 0.0f;
        if (ExtractNumber(sim, "pwm_carrier_hz", &carrier)) {
            config_.pwm_carrier_hz = carrier;
            config_.pwm_scope_enabled = true;
        }
    }
    {
        float live = 0.0f;
        if (ExtractNumber(sim, "live", &live)) config_.live = live != 0.0f;
        const std::string live_s = ExtractString(sim, "live");
        if (live_s == "true" || live_s == "1") config_.live = true;
    }
    {
        float port = 0.0f;
        if (ExtractNumber(sim, "listen_port", &port)) {
            config_.listen_port = static_cast<int>(port);
        }
        const std::string host = ExtractString(sim, "listen_host");
        if (!host.empty()) config_.listen_host = host;
    }
    const std::string trace = ExtractString(sim, "trace_csv");
    if (!trace.empty()) config_.trace_csv = trace;
    {
        bool b = false;
        if (ExtractBool(sim, "demo_fallback", &b)) config_.demo_fallback = b;
    }
    {
        const std::string cfg = ExtractString(sim, "config_file");
        if (!cfg.empty()) config_.config_file = cfg;
    }

    /* ADC sensor error model — absent keys keep the ideal defaults. */
    const std::string adc_obj = ExtractObject(blob, "adc");
    if (!adc_obj.empty()) {
        if (ExtractNumber(adc_obj, "resolution_bits", &v)) {
            if (v < 1.0f) v = 1.0f;
            if (v > 24.0f) v = 24.0f;
            config_.adc.bits = static_cast<unsigned>(v);
        }
        if (ExtractNumber(adc_obj, "vref_v", &v)) config_.adc.vref_v = v;
        if (ExtractNumber(adc_obj, "ref_v", &v)) config_.adc.ref_volts = v;
        if (ExtractNumber(adc_obj, "divider", &v)) config_.adc.divider = v;
        if (ExtractNumber(adc_obj, "sensitivity_v_per_a", &v)) config_.adc.sensitivity_v_per_a = v;
        if (ExtractNumber(adc_obj, "gain_error", &v)) config_.adc.gain_error = v;
        if (ExtractNumber(adc_obj, "offset_u_a", &v)) config_.adc.offset_u_a = v;
        if (ExtractNumber(adc_obj, "offset_v_a", &v)) config_.adc.offset_v_a = v;
        if (ExtractNumber(adc_obj, "noise_std_a", &v)) config_.adc.noise_std_a = v;
    }

    /* CAN: loopback toggle plus scheduled injected frames. */
    const std::string can_obj = ExtractObject(blob, "can");
    if (!can_obj.empty()) {
        bool b = false;
        if (ExtractBool(can_obj, "loopback", &b)) config_.can_loopback = b;
        const std::string frames_arr = ExtractArray(can_obj, "frames");
        for (const std::string& f : SplitArrayObjects(frames_arr)) {
            SimCanInjectFrame frame{};
            uint32_t id = 0;
            if (!ExtractUint(f, "id", &id)) continue;
            frame.id = id;
            float num = 0.0f;
            if (ExtractNumber(f, "bus", &num)) frame.bus = static_cast<uint8_t>(num);
            bool ext = false;
            if (ExtractBool(f, "ext", &ext)) frame.ext = ext;
            if (ExtractNumber(f, "start_s", &num)) frame.start_s = num;
            if (ExtractNumber(f, "time_s", &num)) frame.start_s = num;
            if (ExtractNumber(f, "period_s", &num)) frame.period_s = num;
            frame.dlc = ParseHexPayload(ExtractString(f, "data"), frame.data,
                                        sizeof(frame.data));
            frame.next_fire_s = frame.start_s;
            config_.can_frames.push_back(frame);
        }
    }

    /* Environment temperatures surfaced by the platform temperature APIs. */
    const std::string env_obj = ExtractObject(blob, "environment");
    if (!env_obj.empty()) {
        if (ExtractNumber(env_obj, "motor_temp_c", &v)) config_.motor_temp_c = v;
        if (ExtractNumber(env_obj, "inverter_temp_c", &v)) config_.inverter_temp_c = v;
    }

    /* Simple fault triggers: overcurrent threshold, undervoltage threshold,
     * and a timed DC-link voltage drop. */
    const std::string faults_obj = ExtractObject(blob, "faults");
    if (!faults_obj.empty()) {
        if (ExtractNumber(faults_obj, "overcurrent_a", &v)) config_.overcurrent_a = v;
        if (ExtractNumber(faults_obj, "undervoltage_v", &v)) config_.undervoltage_v = v;
        if (ExtractNumber(faults_obj, "vdc_glitch_time_s", &v)) config_.vdc_glitch_time_s = v;
        if (ExtractNumber(faults_obj, "vdc_glitch_v", &v)) config_.vdc_glitch_v = v;
    }

    const std::string plant_obj = ExtractObject(blob, "plant");
    if (!plant_obj.empty()) {
        const std::string backend = ExtractString(plant_obj, "backend");
        if (!backend.empty()) config_.plant_backend = backend;
        const std::string netlist = ExtractString(plant_obj, "netlist");
        if (!netlist.empty()) config_.ngspice_netlist = netlist;
        const std::string mode = ExtractString(plant_obj, "mode");
        if (!mode.empty()) config_.plant_mode = mode;
        float substeps = 0.0f;
        if (ExtractNumber(plant_obj, "substeps", &substeps)) {
            config_.ngspice_substeps = static_cast<int>(substeps);
        }
    }

    /* dcdc mode default duties: applied each control step unless the graph
     * wrote PWM that tick (see StepOnce). Only used with
     * plant {backend:"ngspice", mode:"dcdc"}.
     * NB: the lenient parser's key search would match the string VALUE
     * "dcdc" (of plant.mode) as an object key, so the three duty keys are
     * searched flat in the whole blob — the names are unique anyway. */
    if (ExtractNumber(blob, "duty_u_pct", &v)) config_.dcdc_duty_u_pct = v;
    if (ExtractNumber(blob, "duty_v_pct", &v)) config_.dcdc_duty_v_pct = v;
    if (ExtractNumber(blob, "duty_w_pct", &v)) config_.dcdc_duty_w_pct = v;

    /* Graph Var node seeds: {"vars": {"TargetHz": 40.0}}, applied after
     * domain init (emitted builds only). */
    const std::string vars_obj = ExtractObject(blob, "vars");
    if (!vars_obj.empty()) {
        EnumerateKv(vars_obj, &config_.graph_vars);
    }

    config_.throttle_a = ParseStimulus(ExtractObject(blob, "throttle_a"));
    config_.throttle_b = ParseStimulus(ExtractObject(blob, "throttle_b"));
    return true;
}

bool SimRuntime::LoadScenario(const char* path) {
    if (!ParseScenario(path)) return false;

    /* Scenario validation: the PMSM plant divides by rs, Ld, Lq and J and the
     * encoder model divides by pole_pairs — a zero/negative value would make
     * the plant state NaN, and the fault checks (max comparisons) then never
     * fire (all NaN comparisons are false), silently defeating protection.
     * Reject at load instead. (The induction model clamps its derived terms
     * in InductionMachine::Step; here the shared params are always checked
     * and the dq-inductances only when the PMSM path would use them.) */
    {
        const MotorParams& m = config_.motor;
        const auto nonpos = [](float x) { return !std::isfinite(x) || x <= 0.0f; };
        std::string bad;
        if (nonpos(m.rs_ohm)) bad = "rs_ohm";
        if (nonpos(m.inertia_kg_m2)) bad = "inertia_kg_m2";
        if (m.pole_pairs < 1) bad = "pole_pairs";
        if (!std::isfinite(m.friction_nm_per_rad_s) ||
            m.friction_nm_per_rad_s < 0.0f) bad = "friction_nm_per_rad_s";
        if (m.machine == MachineType::Pmsm) {
            if (nonpos(m.ld_h)) bad = "ld_h";
            if (nonpos(m.lq_h)) bad = "lq_h";
            if (!std::isfinite(m.flux_wb) || m.flux_wb < 0.0f) bad = "flux_wb";
        } else {
            if (nonpos(m.rr_ohm)) bad = "rr_ohm";
            if (nonpos(m.lm_h)) bad = "lm_h";
            if (nonpos(m.lls_h)) bad = "lls_h";
            if (nonpos(m.llr_h)) bad = "llr_h";
        }
        if (!bad.empty()) {
            std::cerr << "HostSim: ERROR: scenario " << path
                      << " has invalid motor parameter \"" << bad
                      << "\" (must be positive/finite); refusing to run a "
                         "simulation whose protection checks would be inert\n";
            return false;
        }
    }

    tim_dt_s_ = 1.0 / std::max(1.0, static_cast<double>(config_.tim_isr_hz));
    adc_dt_s_ = 1.0 / std::max(1.0, static_cast<double>(config_.adc_isr_hz));
    app_dt_s_ = 1.0 / std::max(1.0, static_cast<double>(config_.app_loop_hz));

    SimAdcConfigure(config_.adc);
    SimCanSetLoopback(config_.can_loopback);
    SimConfigSetBackingFile(config_.config_file.empty()
                                ? nullptr
                                : config_.config_file.c_str());
    demo_fallback_warned_ = false;
    vdc_glitch_applied_ = false;
    overcurrent_raised_ = false;
    undervoltage_raised_ = false;
    for (auto& frame : config_.can_frames) {
        frame.next_fire_s = frame.start_s;
        frame.done = false;
    }

    /* plant.mode validation: only "motor"/"dcdc", only with ngspice backend. */
    if (config_.plant_mode != "motor" && config_.plant_mode != "dcdc") {
        std::cerr << "HostSim: unknown plant.mode \"" << config_.plant_mode
                  << "\" (want \"motor\" or \"dcdc\"); keeping motor\n";
        config_.plant_mode = "motor";
    }
    const bool want_dcdc = (config_.plant_mode == "dcdc");
    if (want_dcdc && config_.plant_backend != "ngspice") {
        std::cerr << "HostSim: plant.mode \"dcdc\" requires plant.backend "
                     "\"ngspice\"; mode ignored\n";
    }

    plant_ = CreatePlantBackend(config_.plant_backend, config_.motor.machine);
    if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
        if (!config_.ngspice_netlist.empty()) {
            /* Netlist paths are relative to the process CWD; as a fallback
             * (so a scenario can be launched from any directory) probe the
             * scenario file's directory first, then the parent's — the
             * emitted layout keeps plants/ next to scenarios/, so a
             * "plants/x.cir" reference resolves via the parent. */
            std::string netlist = config_.ngspice_netlist;
            {
                std::ifstream probe(netlist);
                if (!probe) {
                    const size_t slash = std::string(path).find_last_of("/\\");
                    if (slash != std::string::npos) {
                        const std::string dir =
                            std::string(path).substr(0, slash + 1);
                        const std::string candidates[2] = {dir + netlist,
                                                           dir + "../" + netlist};
                        for (const std::string& alt : candidates) {
                            std::ifstream probe2(alt);
                            if (probe2) {
                                netlist = alt;
                                std::cerr << "HostSim: netlist resolved "
                                             "relative to scenario: "
                                          << netlist << '\n';
                                break;
                            }
                        }
                    }
                }
            }
            ng->SetNetlistPath(netlist);
        }
        ng->SetSubsteps(config_.ngspice_substeps);
        /* Lets LoadNetlist raise a too-short .tran TSTOP to cover the run. */
        ng->SetPlannedDuration(config_.duration_s, config_.live);
        ng->SetMode(want_dcdc && config_.plant_backend == "ngspice"
                        ? NgspicePlantMode::Dcdc
                        : NgspicePlantMode::Motor);
    }
    plant_->SetParams(config_.motor);
    plant_->Reset();

    /* Mode vs netlist class guard (the plant detected the mismatch while
     * loading and already logged specifics): never keep running a mismatched
     * pair — fall back to the ODE plant loudly. A netlist that failed to
     * load gets the same treatment: without it Step() would silently no-op
     * forever with a frozen all-zero state. */
    dcdc_mode_ = false;
    if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
        if (!ng->CircuitLoaded() || !ng->ModeMatchesNetlist()) {
            if (!ng->CircuitLoaded()) {
                std::cerr << "HostSim: ERROR: ngspice backend has no usable "
                             "circuit — falling back to OdePlant\n";
            } else {
                std::cerr << "HostSim: ERROR: plant mode/netlist mismatch — "
                             "falling back to OdePlant\n";
            }
            plant_ = CreatePlantBackend("ode", config_.motor.machine);
            plant_->SetParams(config_.motor);
            plant_->Reset();
        } else {
            dcdc_mode_ = ng->DcdcActive();
        }
    }
    time_s_ = 0.0;
    next_tim_s_ = 0.0;
    next_adc_s_ = 0.0;
    next_app_s_ = 0.0;
    return true;
}

void SimRuntime::OpenTrace() {
    trace_.open(config_.trace_csv, std::ios::out | std::ios::trunc);
    trace_ok_ = static_cast<bool>(trace_);
    if (!trace_ok_) {
        std::cerr << "HostSim: ERROR: cannot open trace " << config_.trace_csv
                  << " — batch results will NOT be recorded\n";
        return;
    }
    trace_ << std::setprecision(8);
    /* dcdc runs extend the fixed motor schema with converter probes; the
     * motor columns still lead (theta_e/omega_e stay 0 in dcdc mode). */
    trace_dcdc_ = false;
    if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
        trace_dcdc_ = ng->DcdcActive();
    }
    trace_ << "time_us,throttle_a,throttle_b,duty_u,duty_v,duty_w,"
              "i_a,i_b,i_c,theta_e,omega_e";
    if (trace_dcdc_) {
        trace_ << ",v_bus1,v_bus2,v_bus3,i_leg1,i_leg2,i_leg3";
    }
    trace_ << '\n';
}

void SimRuntime::InitDomains() {
    if (!config_.live) {
        OpenTrace();
    } else if (config_.telem_hz < 1500.0f) {
        /* Live plots need high sampling rate (~2000 Hz) for smooth waveforms. */
        config_.telem_hz = 2000.0f;
    }
    auto& pwm = GlobalPwmScope();
    pwm.SetCarrierHz(config_.pwm_carrier_hz);
    pwm.SetVdc(config_.motor.vdc_v);
    if (config_.pwm_scope_enabled && config_.live && config_.pwm_telem_hz <= 0.0f) {
        config_.pwm_telem_hz =
            std::clamp(config_.pwm_carrier_hz * 12.0f, 1000.0f, 4000.0f);
    }
    auto& ctx = GetSimContext();
    ctx.vdc_v = config_.motor.vdc_v;
    ctx.plant_vdc_v = config_.motor.vdc_v;
    ctx.motor_temp_c = config_.motor_temp_c;
    ctx.inverter_temp_c = config_.inverter_temp_c;
    ctx.pole_pairs = config_.motor.pole_pairs;
    if (!plant_) {
        plant_ = CreatePlantBackend(config_.plant_backend, config_.motor.machine);
        plant_->SetParams(config_.motor);
        plant_->Reset();
    }
    SimRuntime_RegisterPlant(plant_.get());
    /* Seed the current observer from the scenario motor parameters (the sim's
     * MotorCalibration equivalent) before generated constructors run; a
     * hw.current_observer node's constructor re-applies the same snapshot via
     * platform_observer_init_from_calibration(). */
    SimObserverConfigure(config_.motor.rs_ohm, config_.motor.ld_h,
                         config_.motor.flux_wb,
                         static_cast<float>(config_.motor.pole_pairs));
    next_telem_s_ = 0.0f;
    // RTE_EMIT: app_loop init
    // RTE_EMIT: tim_isr init
    // RTE_EMIT: adc_isr init
    // RTE_EMIT: vsense init
    ApplyGraphVars();
}

void SimRuntime::ApplyGraphVars() {
    if (config_.graph_vars.empty()) return;
#if HOSTSIM_HAS_GENERATED_DOMAINS
    struct DomainVars {
        const RteParamDesc* vars;
        size_t count;
        void* state;
    };
    const DomainVars domains[] = {
        {app::g_tim_isr_vars, app::g_tim_isr_var_count, &appState.tim_isr},
        {app::g_app_loop_vars, app::g_app_loop_var_count, &appState.app_loop},
        {app::g_adc_isr_vars, app::g_adc_isr_var_count, &appState.adc_isr},
        {app::g_vsense_vars, app::g_vsense_var_count, &appState.vsense},
    };
    for (const auto& [name, value] : config_.graph_vars) {
        bool applied = false;
        for (const auto& domain : domains) {
            for (size_t i = 0; i < domain.count; ++i) {
                if (domain.vars[i].name && name == domain.vars[i].name) {
                    domain.vars[i].set(domain.state, value);
                    std::fprintf(stderr, "HostSim: scenario var %.*s = %g\n",
                                 static_cast<int>(name.size()), name.c_str(),
                                 static_cast<double>(value));
                    applied = true;
                }
            }
        }
        if (!applied) {
            std::fprintf(stderr,
                         "HostSim: WARNING: scenario var \"%s\" matches no "
                         "graph Var node; ignored\n",
                         name.c_str());
        }
    }
#else
    std::fprintf(stderr,
                 "HostSim: WARNING: scenario \"vars\" given but this binary "
                 "has no emitted graph domains; var seeds ignored\n");
#endif
}

void SimRuntime::WriteTraceRow() {
    if (!trace_ || !plant_) return;
    const auto& st = plant_->State();
    trace_ << TimeMicros() << ','
           << throttle_a_ << ',' << throttle_b_ << ','
           << duty_u_ << ',' << duty_v_ << ',' << duty_w_ << ','
           << st.ia_a << ',' << st.ib_a << ',' << st.ic_a << ','
           << plant_->ThetaElectricalDeg() << ','
           << plant_->OmegaElectricalRadPerSec();
    if (trace_dcdc_) {
        NgspicePlant::DcdcProbes probes{};
        if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
            ng->GetDcdcProbes(&probes);
        }
        trace_ << ',' << probes.v_bus[0] << ',' << probes.v_bus[1] << ','
               << probes.v_bus[2] << ',' << probes.i_leg[0] << ','
               << probes.i_leg[1] << ',' << probes.i_leg[2];
    }
    trace_ << '\n';
}

bool SimRuntime::StepOnce() {
    if (!config_.live && time_s_ > config_.duration_s) return false;

    throttle_a_ = EvaluateStimulus(config_.throttle_a);
    throttle_b_ = EvaluateStimulus(config_.throttle_b);

    auto& pub = GlobalTelemetryPublisher();
    if (pub.HasThrottleOverrideA()) throttle_a_ = pub.ThrottleOverrideA();
    if (pub.HasThrottleOverrideB()) throttle_b_ = pub.ThrottleOverrideB();

    auto& ctx = GetSimContext();
    ctx.throttle_a = throttle_a_;
    ctx.throttle_b = throttle_b_;
    ctx.time_us = TimeMicros();

    /* Fault injection: timed DC-link voltage drop, seen by both the control
     * code (ctx.vdc_v) and the plant (motor params). Applied once. */
    if (config_.vdc_glitch_time_s >= 0.0f && !vdc_glitch_applied_ &&
        time_s_ + 1e-9 >= config_.vdc_glitch_time_s) {
        vdc_glitch_applied_ = true;
        config_.motor.vdc_v = config_.vdc_glitch_v;
        ctx.vdc_v = config_.vdc_glitch_v;
        ctx.plant_vdc_v = config_.vdc_glitch_v;
        if (plant_) plant_->SetParams(config_.motor);
        std::fprintf(stderr,
                     "HostSim: DC link glitch at t=%.3f s -> vdc=%.2f V\n",
                     static_cast<double>(time_s_),
                     static_cast<double>(config_.vdc_glitch_v));
    }

    if (time_s_ + 1e-9 >= next_tim_s_) {
        ctx.pwm_written = false;
        /* Domain dt for generated code (Gen6 pwm.cpp TIM1 update ISR sets it
         * before the generated step). */
        platform_set_current_domain_dt(static_cast<float>(tim_dt_s_));
        // RTE_EMIT: tim_isr step
        if (!ctx.pwm_written && (throttle_a_ > 0.0f || throttle_b_ > 0.0f)) {
            if (config_.demo_fallback) {
                /* Legacy bring-up behaviour: synthesize open-loop SPWM while no
                 * graph node drives the duties. Opt-in via "demo_fallback":
                 * true because it silently masks emitted graphs that never
                 * call platform_pwm_set. */
                const float freq_hz = throttle_b_ > 0.0f
                                          ? (1.0f + 19.0f * throttle_b_)
                                          : 10.0f;
                platform_spwm_step(throttle_a_, freq_hz,
                                   static_cast<float>(tim_dt_s_),
                                   &ctx.duty_u, &ctx.duty_v, &ctx.duty_w);
            } else if (!demo_fallback_warned_ && !dcdc_mode_) {
                /* Suppressed in dcdc mode: converter legs take their duties
                 * from the scenario "dcdc" keys below, so a silent zero duty
                 * there means the scenario asked for 0%, not a dead graph. */
                demo_fallback_warned_ = true;
                std::fprintf(stderr,
                             "HostSim: throttle is non-zero but no graph node "
                             "is driving platform_pwm_set — all duties stay 0 "
                             "(the legacy SPWM fallback is off; enable with "
                             "\"demo_fallback\": true in the scenario).\n");
            }
        }
        /* dcdc duty defaults: only when nothing drove the PWM outputs this
         * tick. Precedence (highest first): live duty override (applied
         * below) > graph platform_pwm_set (ctx.pwm_written) > scenario
         * "dcdc" duties (here) > legacy demo_fallback (written above,
         * overwritten here). */
        if (dcdc_mode_ && !ctx.pwm_written) {
            ctx.duty_u = config_.dcdc_duty_u_pct;
            ctx.duty_v = config_.dcdc_duty_v_pct;
            ctx.duty_w = config_.dcdc_duty_w_pct;
        }
        duty_u_ = ctx.duty_u;
        duty_v_ = ctx.duty_v;
        duty_w_ = ctx.duty_w;
        if (pub.HasDutyOverrideU()) duty_u_ = pub.DutyOverrideU();
        if (pub.HasDutyOverrideV()) duty_v_ = pub.DutyOverrideV();
        if (pub.HasDutyOverrideW()) duty_w_ = pub.DutyOverrideW();
        ctx.duty_applied_u = duty_u_;
        ctx.duty_applied_v = duty_v_;
        ctx.duty_applied_w = duty_w_;
        if (config_.pwm_scope_enabled) {
            auto& pwm = GlobalPwmScope();
            pwm.SetVdc(ctx.vdc_v);
            pwm.SetDuties(duty_u_, duty_v_, duty_w_);
            pwm.AdvanceInterval(static_cast<float>(tim_dt_s_));
        }
        if (plant_) {
            plant_->Step(duty_u_, duty_v_, duty_w_,
                         static_cast<float>(tim_dt_s_));
            ++ctx.plant_step_seq;

            /* Fault injection: surface limits through platform_raise_fault so
             * graph code sees them via platform_has_critical_fault. */
            if (config_.overcurrent_a > 0.0f && !overcurrent_raised_) {
                const auto& st = plant_->State();
                const float peak = std::max(
                    {std::fabs(st.ia_a), std::fabs(st.ib_a), std::fabs(st.ic_a)});
                if (peak > config_.overcurrent_a) {
                    overcurrent_raised_ = true;
                    /* Gen6 FaultSource::PhaseOvercurrent / Reason::
                     * PhaseOvercurrentSoftware numbering. */
                    platform_raise_fault(128u, 4u);
                    std::fprintf(stderr,
                                 "HostSim: overcurrent fault at t=%.3f s "
                                 "(peak %.2f A > %.2f A)\n",
                                 static_cast<double>(time_s_),
                                 static_cast<double>(peak),
                                 static_cast<double>(config_.overcurrent_a));
                }
            }
            if (config_.undervoltage_v > 0.0f && !undervoltage_raised_ &&
                ctx.vdc_v < config_.undervoltage_v) {
                undervoltage_raised_ = true;
                /* Gen6 FaultSource::Max22530Uv / Reason::PvdTriggered. */
                platform_raise_fault(8u, 17u);
                std::fprintf(stderr,
                             "HostSim: undervoltage fault at t=%.3f s "
                             "(vdc %.2f V < %.2f V)\n",
                             static_cast<double>(time_s_),
                             static_cast<double>(ctx.vdc_v),
                             static_cast<double>(config_.undervoltage_v));
            }
        }
        SimNotifyEncoderSample();
        next_tim_s_ += tim_dt_s_;
    }

    if (time_s_ + 1e-9 >= next_adc_s_) {
        /* Conversion trigger: latch a coherent ADC sample set from the plant
         * before the graph's adc_isr domain reads the injected channels. */
        SimAdcTriggerConversion();
        /* Domain dt for generated code (Gen6 PhaseCurrentADC's injected
         * conversion-complete ISR sets it before the generated step). */
        platform_set_current_domain_dt(static_cast<float>(adc_dt_s_));
        // RTE_EMIT: adc_isr step
        next_adc_s_ += adc_dt_s_;
    }

    if (time_s_ + 1e-9 >= next_app_s_) {
        /* Domain dt for generated code (Gen6 InverterMain sets the app-loop
         * dt before stepping the generated domain). */
        platform_set_current_domain_dt(static_cast<float>(app_dt_s_));
        // RTE_EMIT: app_loop step
        /* Voltage-sense domain (Gen6 InverterMain steps vsense at the same
         * app-loop cadence, with its own 10 ms dt). */
        platform_set_current_domain_dt(0.01f);
        // RTE_EMIT: vsense step
        next_app_s_ += app_dt_s_;
        /* CAN bridge: one non-blocking poll per app-loop tick — accept new
         * spokes, drain reads, inject received frames, run the selftest
         * emitter. Never blocks the sim. */
        GlobalCanBridge().Poll(static_cast<float>(time_s_));
    }

    for (auto& frame : config_.can_frames) {
        if (frame.done) continue;
        if (time_s_ + 1e-9 >= frame.next_fire_s) {
            SimCanInject(frame.bus, frame.id, frame.ext, frame.data,
                         frame.dlc);
            if (frame.period_s > 0.0f) {
                frame.next_fire_s += frame.period_s;
            } else {
                frame.done = true;
            }
        }
    }

    if (time_s_ + 1e-9 >= next_telem_s_) {
        PublishTelemetry();
        const double telem_dt =
            1.0 / std::max(1.0, static_cast<double>(config_.telem_hz));
        next_telem_s_ += telem_dt;
    }

    if (config_.pwm_scope_enabled && EffectivePwmTelemHz() > 0.0f &&
        time_s_ + 1e-9 >= next_pwm_telem_s_) {
        PublishPwmScopeFrame();
        const double pwm_hz = static_cast<double>(EffectivePwmTelemHz());
        next_pwm_telem_s_ += 1.0 / std::max(1.0, pwm_hz);
    }

    if (!config_.live) {
        WriteTraceRow();
    }

    time_s_ += tim_dt_s_;
    if (config_.live) return true;
    return time_s_ <= config_.duration_s;
}

void SimRuntime::PublishPwmScopeTelemetry() {
    auto& pub = GlobalTelemetryPublisher();
    if (!pub.IsListening() || !config_.pwm_scope_enabled) return;
    const auto& pwm = GlobalPwmScope();
    pub.LogF32("pwm_gate_u", pwm.GateU());
    pub.LogF32("pwm_gate_v", pwm.GateV());
    pub.LogF32("pwm_gate_w", pwm.GateW());
    pub.LogF32("pwm_v_u", pwm.VoltageU());
    pub.LogF32("pwm_v_v", pwm.VoltageV());
    pub.LogF32("pwm_v_w", pwm.VoltageW());
    pub.LogF32("pwm_v_uv", pwm.VoltageUV());
    pub.LogF32("pwm_v_vw", pwm.VoltageVW());
    pub.LogF32("pwm_v_wu", pwm.VoltageWU());
}

void SimRuntime::PublishTelemetry() {
    auto& pub = GlobalTelemetryPublisher();
    if (!pub.IsListening() || !plant_) return;
    const auto& st = plant_->State();
    pub.SetBuiltin(throttle_a_, throttle_b_, duty_u_, duty_v_, duty_w_, st.ia_a, st.ib_a,
                   st.ic_a, plant_->ThetaElectricalDeg(), plant_->OmegaElectricalRadPerSec(),
                   GetSimContext().vdc_v);
    pub.LogF32("sim_speed",
               config_.realtime_factor <= 0.0f ? -1.0f : config_.realtime_factor);
    if (dcdc_mode_) {
        NgspicePlant::DcdcProbes probes{};
        if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
            ng->GetDcdcProbes(&probes);
        }
        pub.LogF32("v_bus1", probes.v_bus[0]);
        pub.LogF32("v_bus2", probes.v_bus[1]);
        pub.LogF32("v_bus3", probes.v_bus[2]);
        pub.LogF32("i_leg1", probes.i_leg[0]);
        pub.LogF32("i_leg2", probes.i_leg[1]);
        pub.LogF32("i_leg3", probes.i_leg[2]);
    }
    if (config_.pwm_scope_enabled) {
        pub.LogF32("pwm_telem_hz", EffectivePwmTelemHz());
    }
    if (!config_.pwm_scope_enabled || EffectivePwmTelemHz() <= 0.0f) {
        PublishPwmScopeTelemetry();
    }
    if (config_.pwm_scope_enabled) {
        pub.PublishPlantCycle(static_cast<uint32_t>(TimeMicros()), "pwm_");
    } else {
        pub.PublishCycle(static_cast<uint32_t>(TimeMicros()));
    }
}

void SimRuntime::PaceRealtimeWallClock() const {
    if (config_.realtime_factor <= 0.0f) return;

    const double sim_delta_s = time_s_ - sim_anchor_s_;
    const auto target =
        wall_anchor_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(sim_delta_s /
                                                           static_cast<double>(
                                                               config_.realtime_factor)));
    auto now = std::chrono::steady_clock::now();
    if (now >= target) return;

    const auto remaining = target - now;
    constexpr auto kSpinThreshold = std::chrono::milliseconds(2);
    if (remaining > kSpinThreshold) {
        std::this_thread::sleep_for(remaining - kSpinThreshold);
    }
    while (std::chrono::steady_clock::now() < target) {
        std::this_thread::yield();
    }
}

int SimRuntime::Run() {
    InitDomains();
    GlobalCanBridge().Start();

    if (config_.live) {
        auto& pub = GlobalTelemetryPublisher();
        if (!pub.Start(config_.listen_host, config_.listen_port)) {
            std::cerr << "HostSim: failed to listen on " << config_.listen_host << ':'
                      << config_.listen_port << '\n';
            return 1;
        }
        std::printf("HostSim live: realtime_factor=%.2f telem_hz=%.0f\n",
                    static_cast<double>(config_.realtime_factor),
                    static_cast<double>(config_.telem_hz));
        if (config_.pwm_scope_enabled) {
            std::printf("HostSim live: PWM scope carrier=%.0f Hz base_telem=%.0f Hz (scales with speed)\n",
                        static_cast<double>(config_.pwm_carrier_hz),
                        static_cast<double>(config_.pwm_telem_hz));
        }
        std::printf("HostSim live: commands via NodeGUI console: throttle a 0.5 | duty u 60 | pause | clear | quit\n");
        std::fflush(stdout);

        RealtimeSession realtime_session;
        /* CLI --live reaches here after LoadScenario already loaded the
         * netlist, so the TSTOP auto-raise saw live=false; warn loudly that
         * the analysis will still stop (loudly) at the card's TSTOP. */
        if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
            const double tstop = ng->NetlistTstopSeconds();
            if (tstop > 0.0 && tstop < 1.0e8) {
                std::fprintf(stderr,
                             "HostSim: WARNING: live mode with ngspice netlist "
                             ".tran TSTOP=%.6g s — the analysis halts loudly "
                             "and the plant freezes once sim time passes that "
                             "mark; raise .tran TSTOP for long sessions\n",
                             tstop);
            }
        }
        ResetWallClockAnchor();
        double last_pace_sim_s = 0.0;
        constexpr double kPaceIntervalSimS = 0.001;
        bool was_paused = false;
        while (true) {
            if (!pub.PollCommands()) break;

            if (pub.IsPaused()) {
                if (!was_paused) {
                    PublishTelemetry();
                    was_paused = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            was_paused = false;

            if (!StepOnce()) break;

            if (config_.realtime_factor > 0.0f &&
                (time_s_ - last_pace_sim_s) >= kPaceIntervalSimS) {
                PaceRealtimeWallClock();
                last_pace_sim_s = time_s_;
            }
        }
        pub.Stop();
        Shutdown();
        std::printf("HostSim live: stopped at t=%.3f s\n", static_cast<double>(time_s_));
        return 0;
    }

    while (StepOnce()) {
    }
    Shutdown();
    if (!trace_ok_) {
        std::fprintf(stderr,
                     "HostSim: ERROR: trace %s was not written (open failed "
                     "earlier); exiting nonzero\n",
                     config_.trace_csv.c_str());
        return 1;
    }
    std::printf("HostSim: wrote %s\n", config_.trace_csv.c_str());
    return 0;
}

void SimRuntime::Shutdown() {
    GlobalCanBridge().Shutdown();
    if (trace_.is_open()) trace_.close();
    SimConfigPersist();
}

} // namespace hostsim
