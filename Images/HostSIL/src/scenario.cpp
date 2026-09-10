/*
 * scenario.cpp — hand-rolled JSON extraction in the same style as
 * Images/HostSim/src/sim_runtime.cpp (Trim/ExtractString/ExtractNumber/
 * ExtractObject), extended with a flat object enumerator for
 * firmware_config KV seeds.
 */
#include "scenario.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sil {

float ScalarProfile::at(float t_s) const {
    if (type == "ramp") {
        if (t_s <= start_s) return start;
        if (t_s >= end_s) return end;
        if (end_s <= start_s) return end;
        const float f = (t_s - start_s) / (end_s - start_s);
        return start + f * (end - start);
    }
    if (type == "step") {
        return (t_s >= step_time_s) ? step_value : value;
    }
    return value;
}

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

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

bool ExtractBool(const std::string& blob, const std::string& key, bool* out) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = blob.find(needle);
    if (pos == std::string::npos) return false;
    const size_t colon = blob.find(':', pos);
    if (colon == std::string::npos) return false;
    const std::string rest = Trim(blob.substr(colon + 1));
    if (rest.rfind("true", 0) == 0) { *out = true; return true; }
    if (rest.rfind("false", 0) == 0) { *out = false; return true; }
    return false;
}

/* Enumerate "key": number pairs in a flat object blob.
 * String values would otherwise alias the next pair's colon (a value in
 * quotes is not a number): skip them instead. */
void EnumerateKv(const std::string& blob,
                 std::vector<std::pair<std::string, float>>& out) {
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
            out.emplace_back(key, v);
        }
        i = colon + 1;
    }
}

/* Enumerate "key": "string-value" pairs in a flat object blob — the string
 * counterpart of EnumerateKv (used for the "commands" schedule). */
void EnumerateStringKv(const std::string& blob,
                       std::vector<std::pair<std::string, std::string>>& out) {
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
        if (start >= blob.size() || blob[start] != '"') {
            /* Non-string value: let EnumerateKv-style parsing own it. */
            i = colon + 1;
            continue;
        }
        const size_t vend = blob.find('"', start + 1);
        if (vend == std::string::npos) break;
        out.emplace_back(key, blob.substr(start + 1, vend - start - 1));
        i = vend + 1;
    }
}

ScalarProfile ParseProfile(const std::string& blob) {
    ScalarProfile p{};
    const std::string t = ExtractString(blob, "type");
    if (t == "ramp") p.type = "ramp";
    else if (t == "step") p.type = "step";
    else p.type = "constant";
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

bool LoadScenario(const char* path, Scenario& out, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = std::string("cannot open scenario: ") + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string blob = ss.str();

    float v = 0.0f;
    bool b = false;

    const std::string motor = ExtractObject(blob, "motor");
    if (!motor.empty()) {
        if (ExtractNumber(motor, "rs_ohm", &v)) out.rs_ohm = v;
        if (ExtractNumber(motor, "ld_h", &v)) out.ld_h = v;
        if (ExtractNumber(motor, "lq_h", &v)) out.lq_h = v;
        if (ExtractNumber(motor, "flux_wb", &v)) out.flux_wb = v;
        if (ExtractNumber(motor, "pole_pairs", &v)) out.pole_pairs = (int)v;
        if (ExtractNumber(motor, "inertia_kg_m2", &v)) out.inertia_kg_m2 = v;
        if (ExtractNumber(motor, "friction_nm_per_rad_s", &v)) out.friction_nm_per_rad_s = v;
        if (ExtractNumber(motor, "vdc_v", &v)) out.vdc_v = v;
        const std::string machine = ExtractString(motor, "machine");
        if (!machine.empty()) out.machine = machine;
        if (ExtractNumber(motor, "rr_ohm", &v)) out.rr_ohm = v;
        if (ExtractNumber(motor, "lm_h", &v)) out.lm_h = v;
        if (ExtractNumber(motor, "lls_h", &v)) out.lls_h = v;
        if (ExtractNumber(motor, "llr_h", &v)) out.llr_h = v;
    }

    const std::string sim = ExtractObject(blob, "simulation");
    if (!sim.empty()) {
        if (ExtractNumber(sim, "duration_s", &v)) out.duration_s = v;
        if (ExtractNumber(sim, "app_loop_hz", &v)) out.app_loop_hz = v;
        const std::string csv = ExtractString(sim, "trace_csv");
        if (!csv.empty()) out.trace_csv = csv;
        if (ExtractNumber(sim, "trace_decim_us", &v)) out.trace_decim_us = v;
        if (ExtractNumber(sim, "pwm_switching_hz", &v)) out.pwm_switching_hz = v;
        /* Accepted for schema compatibility with HostSim; the SIL scheduler
         * derives the actual ISR rates from the firmware's PWM state. */
    }

    const std::string thr_a = ExtractObject(blob, "throttle_a");
    if (!thr_a.empty()) out.throttle_a = ParseProfile(thr_a);
    const std::string thr_b = ExtractObject(blob, "throttle_b");
    if (!thr_b.empty()) {
        out.throttle_b = ParseProfile(thr_b);
        out.throttle_b_set = true;
    }

    const std::string ctl = ExtractObject(blob, "control");
    if (!ctl.empty()) {
        if (ExtractBool(ctl, "start", &b)) out.control_start = b;
        if (ExtractNumber(ctl, "iq_a", &v)) out.iq_a = v;
        if (ExtractNumber(ctl, "id_a", &v)) out.id_a = v;
        if (ExtractNumber(ctl, "start_time_s", &v)) out.control_start_time_s = v;
    }

    const std::string cfg = ExtractObject(blob, "firmware_config");
    if (!cfg.empty()) {
        EnumerateKv(cfg, out.firmware_config);
    }

    const std::string faults = ExtractObject(blob, "faults");
    if (!faults.empty()) {
        if (ExtractNumber(faults, "vdc_glitch_time_s", &v)) out.vdc_glitch_time_s = v;
        if (ExtractNumber(faults, "vdc_glitch_duration_s", &v)) out.vdc_glitch_duration_s = v;
        if (ExtractNumber(faults, "vdc_glitch_v", &v)) out.vdc_glitch_v = v;
        if (ExtractNumber(faults, "oc_inject_time_s", &v)) out.oc_inject_time_s = v;
        if (ExtractNumber(faults, "oc_inject_duration_s", &v)) out.oc_inject_duration_s = v;
        if (ExtractNumber(faults, "oc_inject_phase", &v)) out.oc_inject_phase = (int)v;
        if (ExtractNumber(faults, "oc_inject_a", &v)) out.oc_inject_a = v;
        if (ExtractNumber(faults, "encoder_freeze_time_s", &v)) out.encoder_freeze_time_s = v;
        if (ExtractNumber(faults, "encoder_freeze_duration_s", &v)) out.encoder_freeze_duration_s = v;
        if (ExtractNumber(faults, "encoder_loss_time_s", &v)) out.encoder_loss_time_s = v;
        if (ExtractNumber(faults, "encoder_loss_duration_s", &v)) out.encoder_loss_duration_s = v;
        if (ExtractNumber(faults, "temp_spike_time_s", &v)) out.temp_spike_time_s = v;
        if (ExtractNumber(faults, "temp_spike_duration_s", &v)) out.temp_spike_duration_s = v;
        if (ExtractNumber(faults, "temp_spike_channel", &v)) out.temp_spike_channel = (int)v;
        if (ExtractNumber(faults, "temp_spike_c", &v)) out.temp_spike_c = v;
    }

    const std::string commands = ExtractObject(blob, "commands");
    if (!commands.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        EnumerateStringKv(commands, kv);
        for (const auto& [k, line] : kv) {
            char* end = nullptr;
            const float t = std::strtof(k.c_str(), &end);
            if (end != k.c_str() && *end == '\0') {
                out.commands.emplace_back(t, line);
            }
        }
    }

    const std::string fram = ExtractString(blob, "fram_image");
    if (!fram.empty()) out.fram_image = fram;

    return true;
}

} // namespace sil
