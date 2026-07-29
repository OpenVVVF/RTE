#include "sim_runtime.h"

#include "AppState.h"
#include "sim_context.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace hostsim {
namespace {

SimRuntime g_runtime;

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

float SimRuntime::EvaluateStimulus(const StimulusProfile& profile) const {
    switch (profile.type) {
    case StimulusType::Constant:
        return profile.value;
    case StimulusType::Ramp:
        if (time_s_ <= profile.start_s) return profile.start;
        if (time_s_ >= profile.end_s) return profile.end;
        if (profile.end_s <= profile.start_s) return profile.end;
        {
            const float t = (time_s_ - profile.start_s) /
                            (profile.end_s - profile.start_s);
            return profile.start + t * (profile.end - profile.start);
        }
    case StimulusType::Step:
        return time_s_ >= profile.step_time_s ? profile.step_value
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

    if (ExtractNumber(sim, "duration_s", &v)) config_.duration_s = v;
    if (ExtractNumber(sim, "tim_isr_hz", &v)) config_.tim_isr_hz = v;
    if (ExtractNumber(sim, "adc_isr_hz", &v)) config_.adc_isr_hz = v;
    if (ExtractNumber(sim, "app_loop_hz", &v)) config_.app_loop_hz = v;
    const std::string trace = ExtractString(sim, "trace_csv");
    if (!trace.empty()) config_.trace_csv = trace;

    config_.throttle_a = ParseStimulus(ExtractObject(blob, "throttle_a"));
    config_.throttle_b = ParseStimulus(ExtractObject(blob, "throttle_b"));
    return true;
}

bool SimRuntime::LoadScenario(const char* path) {
    if (!ParseScenario(path)) return false;
    tim_dt_s_ = 1.0f / std::max(1.0f, config_.tim_isr_hz);
    adc_dt_s_ = 1.0f / std::max(1.0f, config_.adc_isr_hz);
    app_dt_s_ = 1.0f / std::max(1.0f, config_.app_loop_hz);
    motor_.SetParams(config_.motor);
    motor_.Reset();
    time_s_ = 0.0f;
    next_tim_s_ = 0.0f;
    next_adc_s_ = 0.0f;
    next_app_s_ = 0.0f;
    return true;
}

void SimRuntime::OpenTrace() {
    trace_.open(config_.trace_csv, std::ios::out | std::ios::trunc);
    if (!trace_) {
        std::cerr << "HostSim: cannot open trace " << config_.trace_csv << '\n';
        return;
    }
    trace_ << "time_us,throttle_a,throttle_b,duty_u,duty_v,duty_w,"
              "i_a,i_b,i_c,theta_e,omega_e\n";
}

void SimRuntime::InitDomains() {
    OpenTrace();
    GetSimContext().vdc_v = config_.motor.vdc_v;
    SimRuntime_RegisterMotor(&motor_);
    // RTE_EMIT: app_loop init
    // RTE_EMIT: tim_isr init
    // RTE_EMIT: adc_isr init
}

void SimRuntime::WriteTraceRow() {
    if (!trace_) return;
    const auto& st = motor_.State();
    trace_ << TimeMicros() << ','
           << throttle_a_ << ',' << throttle_b_ << ','
           << duty_u_ << ',' << duty_v_ << ',' << duty_w_ << ','
           << st.ia_a << ',' << st.ib_a << ',' << st.ic_a << ','
           << motor_.ThetaElectricalDeg() << ','
           << motor_.OmegaElectricalRadPerSec() << '\n';
}

bool SimRuntime::StepOnce() {
    if (time_s_ > config_.duration_s) return false;

    throttle_a_ = EvaluateStimulus(config_.throttle_a);
    throttle_b_ = EvaluateStimulus(config_.throttle_b);
    auto& ctx = GetSimContext();
    ctx.throttle_a = throttle_a_;
    ctx.throttle_b = throttle_b_;
    ctx.time_us = TimeMicros();

    if (time_s_ + 1e-9f >= next_tim_s_) {
        // RTE_EMIT: tim_isr step
        duty_u_ = ctx.duty_u;
        duty_v_ = ctx.duty_v;
        duty_w_ = ctx.duty_w;
        motor_.Step(duty_u_, duty_v_, duty_w_, tim_dt_s_);
        SimNotifyEncoderSample();
        next_tim_s_ += tim_dt_s_;
    }

    if (time_s_ + 1e-9f >= next_adc_s_) {
        // RTE_EMIT: adc_isr step
        next_adc_s_ += adc_dt_s_;
    }

    if (time_s_ + 1e-9f >= next_app_s_) {
        // RTE_EMIT: app_loop step
        next_app_s_ += app_dt_s_;
    }

    WriteTraceRow();
    time_s_ += tim_dt_s_;
    return time_s_ <= config_.duration_s;
}

void SimRuntime::Shutdown() {
    if (trace_.is_open()) trace_.close();
}

} // namespace hostsim
