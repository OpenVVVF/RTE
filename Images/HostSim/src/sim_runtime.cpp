#include "sim_runtime.h"

#include "AppState.h"
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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "realtime_platform.h"

namespace hostsim {

SimRuntime::SimRuntime() : plant_(std::make_unique<OdePlant>()) {}
SimRuntime::~SimRuntime() = default;

namespace {

SimRuntime g_runtime;

uint64_t g_prof_plant_us = 0;
uint64_t g_prof_telem_us = 0;

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

    const std::string plant_obj = ExtractObject(blob, "plant");
    if (!plant_obj.empty()) {
        const std::string backend = ExtractString(plant_obj, "backend");
        if (!backend.empty()) config_.plant_backend = backend;
        const std::string netlist = ExtractString(plant_obj, "netlist");
        if (!netlist.empty()) config_.ngspice_netlist = netlist;
        float substeps = 0.0f;
        if (ExtractNumber(plant_obj, "substeps", &substeps)) {
            config_.ngspice_substeps = static_cast<int>(substeps);
        }
    }

    config_.throttle_a = ParseStimulus(ExtractObject(blob, "throttle_a"));
    config_.throttle_b = ParseStimulus(ExtractObject(blob, "throttle_b"));
    return true;
}

bool SimRuntime::LoadScenario(const char* path) {
    if (!ParseScenario(path)) return false;
    tim_dt_s_ = 1.0f / std::max(1.0f, config_.tim_isr_hz);
    adc_dt_s_ = 1.0f / std::max(1.0f, config_.adc_isr_hz);
    app_dt_s_ = 1.0f / std::max(1.0f, config_.app_loop_hz);

    RecreatePlant();
    time_s_ = 0.0f;
    next_tim_s_ = 0.0f;
    next_adc_s_ = 0.0f;
    next_app_s_ = 0.0f;
    return true;
}

void SimRuntime::RecreatePlant() {
    plant_ = CreatePlantBackend(config_.plant_backend);
    if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
        std::string netlist = config_.ngspice_netlist;
        if (netlist.empty()) {
            // Default RL load netlist so --plant-backend ngspice works from
            // any scenario that does not explicitly name one.
            netlist = "plants/inverter_rl.cir";
        }
        ng->SetNetlistPath(netlist);
        ng->SetSubsteps(config_.ngspice_substeps);
    }
    plant_->SetParams(config_.motor);
    plant_->Reset();
    SimRuntime_RegisterPlant(plant_.get());
}

void SimRuntime::SetPlantBackend(const std::string& backend) {
    if (backend == config_.plant_backend) return;
    config_.plant_backend = backend;
    RecreatePlant();
}

void SimRuntime::SetTimIsrHz(float hz) {
    config_.tim_isr_hz = hz;
    tim_dt_s_ = 1.0f / std::max(1.0f, config_.tim_isr_hz);
}

void SimRuntime::SetNgspiceSubsteps(int substeps) {
    config_.ngspice_substeps = substeps;
    if (auto* ng = dynamic_cast<NgspicePlant*>(plant_.get())) {
        ng->SetSubsteps(substeps);
    }
}

void SimRuntime::OpenTrace() {
    trace_.open(config_.trace_csv, std::ios::out | std::ios::trunc);
    if (!trace_) {
        std::cerr << "HostSim: cannot open trace " << config_.trace_csv << '\n';
        return;
    }
    trace_ << std::setprecision(8);
    trace_ << "time_us,throttle_a,throttle_b,duty_u,duty_v,duty_w,"
              "i_a,i_b,i_c,theta_e,omega_e\n";
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
    GetSimContext().vdc_v = config_.motor.vdc_v;
    if (!plant_) {
        plant_ = CreatePlantBackend(config_.plant_backend);
        plant_->SetParams(config_.motor);
        plant_->Reset();
    }
    SimRuntime_RegisterPlant(plant_.get());
    next_telem_s_ = 0.0f;
    // RTE_EMIT: app_loop init
    // RTE_EMIT: tim_isr init
    // RTE_EMIT: adc_isr init
}

void SimRuntime::WriteTraceRow() {
    if (!trace_ || !plant_) return;
    const auto& st = plant_->State();
    trace_ << TimeMicros() << ','
           << throttle_a_ << ',' << throttle_b_ << ','
           << duty_u_ << ',' << duty_v_ << ',' << duty_w_ << ','
           << st.ia_a << ',' << st.ib_a << ',' << st.ic_a << ','
           << plant_->ThetaElectricalDeg() << ','
           << plant_->OmegaElectricalRadPerSec() << '\n';
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

    if (time_s_ + 1e-9f >= next_tim_s_) {
        // RTE_EMIT: tim_isr step
        if (ctx.duty_u == 0.0f && ctx.duty_v == 0.0f && ctx.duty_w == 0.0f) {
            if (throttle_a_ > 0.0f || throttle_b_ > 0.0f) {
                float freq_hz = throttle_b_ > 0.0f ? (1.0f + 19.0f * throttle_b_) : 10.0f;
                platform_spwm_step(throttle_a_, freq_hz, tim_dt_s_, &ctx.duty_u, &ctx.duty_v, &ctx.duty_w);
            }
        }
        duty_u_ = ctx.duty_u;
        duty_v_ = ctx.duty_v;
        duty_w_ = ctx.duty_w;
        if (pub.HasDutyOverrideU()) duty_u_ = pub.DutyOverrideU();
        if (pub.HasDutyOverrideV()) duty_v_ = pub.DutyOverrideV();
        if (pub.HasDutyOverrideW()) duty_w_ = pub.DutyOverrideW();
        if (config_.pwm_scope_enabled) {
            auto& pwm = GlobalPwmScope();
            pwm.SetVdc(ctx.vdc_v);
            pwm.SetDuties(duty_u_, duty_v_, duty_w_);
            pwm.AdvanceInterval(tim_dt_s_);
        }
        if (plant_) {
            const auto p0 = std::chrono::steady_clock::now();
            plant_->Step(duty_u_, duty_v_, duty_w_, tim_dt_s_);
            g_prof_plant_us += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - p0)
                    .count());
        }
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

    if (time_s_ + 1e-9f >= next_telem_s_) {
        const auto p0 = std::chrono::steady_clock::now();
        PublishTelemetry();
        g_prof_telem_us += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - p0)
                .count());
        const float telem_dt = 1.0f / std::max(1.0f, config_.telem_hz);
        next_telem_s_ += telem_dt;
    }

    if (config_.pwm_scope_enabled && EffectivePwmTelemHz() > 0.0f &&
        time_s_ + 1e-9f >= next_pwm_telem_s_) {
        PublishPwmScopeFrame();
        const float pwm_hz = EffectivePwmTelemHz();
        next_pwm_telem_s_ += 1.0f / std::max(1.0f, pwm_hz);
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

    const double sim_delta_s = static_cast<double>(time_s_ - sim_anchor_s_);
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
        ResetWallClockAnchor();
        float last_pace_sim_s = 0.0f;
        constexpr float kPaceIntervalSimS = 0.001f;
        bool was_paused = false;
        const bool prof = std::getenv("HOSTSIM_NGSPICE_DEBUG") != nullptr;
        uint64_t prof_poll_us = 0, prof_step_us = 0, prof_pace_us = 0, prof_ticks = 0;
        uint64_t prof_iters = 0;
        auto prof_start = std::chrono::steady_clock::now();
        while (true) {
            ++prof_iters;
            const auto iter_start = std::chrono::steady_clock::now();
            const auto t0 = iter_start;
            if (!pub.PollCommands()) {
                if (prof) {
                    const auto ms = [](auto d) {
                        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
                    };
                    std::fprintf(stderr,
                                 "[simprof] break: PollCommands false at iter %llu "
                                 "(t=%lldms, iter_age=%lldms)\n",
                                 static_cast<unsigned long long>(prof_iters),
                                 static_cast<long long>(ms(iter_start - prof_start)),
                                 static_cast<long long>(ms(std::chrono::steady_clock::now() - iter_start)));
                }
                break;
            }
            const auto t1 = std::chrono::steady_clock::now();

            if (pub.IsPaused()) {
                if (!was_paused) {
                    PublishTelemetry();
                    was_paused = true;
                    if (prof) std::fprintf(stderr, "[simprof] PAUSED branch entered\n");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            was_paused = false;

            if (!StepOnce()) {
                if (prof) std::fprintf(stderr, "[simprof] break: StepOnce false at iter %llu\n",
                                       static_cast<unsigned long long>(prof_iters));
                break;
            }
            const auto t2 = std::chrono::steady_clock::now();

            if (config_.realtime_factor > 0.0f &&
                (time_s_ - last_pace_sim_s) >= kPaceIntervalSimS) {
                PaceRealtimeWallClock();
                last_pace_sim_s = time_s_;
            }
            if (prof) {
                const auto t3 = std::chrono::steady_clock::now();
                using Us = std::chrono::microseconds;
                prof_poll_us += static_cast<uint64_t>(std::chrono::duration_cast<Us>(t1 - t0).count());
                prof_step_us += static_cast<uint64_t>(std::chrono::duration_cast<Us>(t2 - t1).count());
                prof_pace_us += static_cast<uint64_t>(std::chrono::duration_cast<Us>(t3 - t2).count());
                if (++prof_ticks % 1000 == 0) {
                    std::fprintf(stderr,
                                 "[simprof] ticks=%llu wall=%.2fs poll=%.1fus step=%.1fus pace=%.1fus plant=%.1fus telem=%.1fus (per tick)\n",
                                 static_cast<unsigned long long>(prof_ticks),
                                 std::chrono::duration<double>(t3 - prof_start).count(),
                                 static_cast<double>(prof_poll_us) / prof_ticks,
                                 static_cast<double>(prof_step_us) / prof_ticks,
                                 static_cast<double>(prof_pace_us) / prof_ticks,
                                 static_cast<double>(g_prof_plant_us) / prof_ticks,
                                 static_cast<double>(g_prof_telem_us) / prof_ticks);
                }
            }
        }
        if (prof) {
            std::fprintf(stderr,
                         "[simprof] loop exited: iters=%llu ticks=%llu paused=%d\n",
                         static_cast<unsigned long long>(prof_iters),
                         static_cast<unsigned long long>(prof_ticks),
                         pub.IsPaused() ? 1 : 0);
        }
        pub.Stop();
        Shutdown();
        std::printf("HostSim live: stopped at t=%.3f s\n", static_cast<double>(time_s_));
        return 0;
    }

    while (StepOnce()) {
    }
    Shutdown();
    std::printf("HostSim: wrote %s\n", config_.trace_csv.c_str());
    return 0;
}

void SimRuntime::Shutdown() {
    if (trace_.is_open()) trace_.close();
}

} // namespace hostsim
