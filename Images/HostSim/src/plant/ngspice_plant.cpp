#include "ngspice_plant.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <strings.h>
#endif

namespace hostsim {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kPhase120Rad = 2.0f * kPi / 3.0f;
constexpr float kSqrt3 = 1.7320508075688772f;

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

float WrapAngle(float theta) {
    while (theta >= kTwoPi) theta -= kTwoPi;
    while (theta < 0.0f) theta += kTwoPi;
    return theta;
}

float ClampDuty(float duty_pct) {
    return std::max(0.0f, std::min(100.0f, duty_pct));
}

void DutiesToAbcVoltage(float du, float dv, float dw, float vdc,
                        float* va, float* vb, float* vc) {
    const float scale = vdc / 100.0f;
    if (va) *va = ClampDuty(du) * scale;
    if (vb) *vb = ClampDuty(dv) * scale;
    if (vc) *vc = ClampDuty(dw) * scale;
}

int CaseInsensitiveCompare(const char* a, const char* b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

} // namespace

NgspicePlant::NgspicePlant() {
    if (std::getenv("HOSTSIM_NGSPICE_SYNC_JUMP")) sync_override_ = true;
    if (LoadSharedLibrary() && BindSymbols()) {
        sharedspice_loaded_ = true;
        int ident = 0;
        fn_ngSpice_Init_(CallbackSendChar, CallbackSendStat, CallbackControlledExit,
                 CallbackSendData, CallbackSendInitData, CallbackBGThreadRunning,
                 this);
        fn_ngSpice_Init_Sync_(CallbackGetVSRCData, CallbackGetISRCData,
                      CallbackGetSyncData, &ident, this);
        std::cerr << "HostSim: libngspice loaded; experimental ngspice plant "
                     "backend active\n";
    } else {
        sharedspice_loaded_ = false;
        UnloadSharedLibrary();
    }
}

NgspicePlant::~NgspicePlant() { UnloadSharedLibrary(); }

bool NgspicePlant::LoadSharedLibrary() {
#if defined(_WIN32)
    lib_handle_ = static_cast<void*>(LoadLibraryA("ngspice.dll"));
#else
    // The runtime package on most Linux distros ships only the versioned
    // soname, so try the linker name first, then the soname.
    static const char* const kCandidates[] = {"libngspice.so", "libngspice.so.0"};
    for (const char* name : kCandidates) {
        lib_handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib_handle_) break;
    }
#endif
    return lib_handle_ != nullptr;
}

void NgspicePlant::UnloadSharedLibrary() {
    if (!lib_handle_) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib_handle_));
#else
    dlclose(lib_handle_);
#endif
    lib_handle_ = nullptr;
}

void NgspicePlant::Command(const char* cmd) {
    if (!fn_ngSpice_Command_ || !cmd) return;
    std::string buffer(cmd);
    fn_ngSpice_Command_(buffer.data());
}

bool NgspicePlant::BindSymbols() {
    if (!lib_handle_) return false;

#define BIND(name)                                                             \
    do {                                                                       \
        fn_##name##_ = reinterpret_cast<FN_##name>(                            \
            GetProcAddress(static_cast<HMODULE>(lib_handle_), #name));         \
        if (!fn_##name##_) {                                                   \
            std::cerr << "HostSim: failed to resolve symbol " #name "\n";      \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if !defined(_WIN32)
#undef BIND
#define BIND(name)                                                             \
    do {                                                                       \
        fn_##name##_ = reinterpret_cast<FN_##name>(dlsym(lib_handle_, #name)); \
        if (!fn_##name##_) {                                                   \
            std::cerr << "HostSim: failed to resolve symbol " #name "\n";      \
            return false;                                                      \
        }                                                                      \
    } while (0)
#endif

    BIND(ngSpice_Init);
    BIND(ngSpice_Init_Sync);
    BIND(ngSpice_Command);
    BIND(ngSpice_Circ);
    BIND(ngSpice_running);
    BIND(ngSpice_CurPlot);
    BIND(ngSpice_AllPlots);
    BIND(ngSpice_AllVecs);
    BIND(ngSpice_SetBkpt);
    BIND(ngGet_Vec_Info);

#undef BIND
    return true;
}

void NgspicePlant::SetParams(const MotorParams& params) { params_ = params; }

void NgspicePlant::Reset() {
    state_ = MotorState{};
    current_sim_time_ = 0.0;
    first_step_ = true;
    analysis_failed_ = false;
    sync_target_time_.store(0.0);
    sync_redo_block_.store(false);

    if (!sharedspice_loaded_) return;

    if (!circuit_loaded_) {
        LoadNetlist();
    }

    ApplyParams();

    if (circuit_loaded_) {
        // Drop any debug stop that survived a previous run before restarting
        // the analysis; ngspice complains ("no debugs in effect") if the
        // delete is issued with none active.
        if (stop_active_) {
            Command("delete all");
            stop_active_ = false;
        }
        Command("reset");
    }
}

float NgspicePlant::ThetaElectricalDeg() const {
    return state_.theta_e_rad * 180.0f / kPi;
}

bool NgspicePlant::DetectBackEmfSources(
    const std::vector<std::string>& lines) {
    static const char* const kNames[3] = {"veu", "vev", "vew"};
    bool found[3] = {false, false, false};
    // ngspice lowercases netlist names internally, so match
    // case-insensitively.
    for (const auto& raw : lines) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '*') continue;
        for (auto& c : line) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (line.find("external") == std::string::npos) continue;
        std::istringstream iss(line);
        std::string name;
        iss >> name;
        for (int k = 0; k < 3; ++k) {
            if (name == kNames[k]) found[k] = true;
        }
    }
    return found[0] && found[1] && found[2];
}

void NgspicePlant::LoadNetlist() {
    if (netlist_path_.empty() || !fn_ngSpice_Circ_) return;

    std::ifstream in(netlist_path_);
    if (!in) {
        std::cerr << "HostSim: cannot open netlist " << netlist_path_ << '\n';
        return;
    }

    std::vector<std::string> storage;
    std::string line;
    while (std::getline(in, line)) {
        if (Trim(line).empty()) continue;
        storage.push_back(line);
    }

    has_bemf_sources_ = DetectBackEmfSources(storage);
    if (has_bemf_sources_) {
        std::cerr << "HostSim: netlist exposes Veu/Vev/Vew back-EMF sources; "
                     "back-EMF is driven in-circuit\n";
    }

    std::vector<char*> lines;
    lines.reserve(storage.size() + 1);
    for (auto& s : storage) {
        lines.push_back(s.data());
    }
    lines.push_back(nullptr);

    if (fn_ngSpice_Circ_(lines.data()) == 0) {
        circuit_loaded_ = true;
    } else {
        std::cerr << "HostSim: ngSpice_Circ failed to load netlist\n";
        circuit_loaded_ = false;
    }
}

void NgspicePlant::ApplyParams() {
    if (!fn_ngSpice_Command_ || !circuit_loaded_) return;

    auto alter = [this](const char* name, double value) {
        std::ostringstream oss;
        oss << "alterparam " << name << "=" << value;
        std::string cmd = oss.str();
        fn_ngSpice_Command_(cmd.data());
    };

    alter("rs", params_.rs_ohm);
    alter("ls", 0.5 * (params_.ld_h + params_.lq_h));
    alter("vdc", params_.vdc_v);
}

void NgspicePlant::UpdatePendingVoltages(float du_pct, float dv_pct,
                                          float dw_pct) {
    const float theta = state_.theta_e_rad;
    const float omega = state_.omega_e_rad_s;
    const float e_peak = params_.flux_wb * omega;

    float va = 0.0f;
    float vb = 0.0f;
    float vc = 0.0f;
    DutiesToAbcVoltage(du_pct, dv_pct, dw_pct, params_.vdc_v, &va, &vb, &vc);

    const float ea = -e_peak * std::sin(theta);
    const float eb = -e_peak * std::sin(theta - kPhase120Rad);
    const float ec = -e_peak * std::sin(theta + kPhase120Rad);

    const float vn = (va + vb + vc) / 3.0f;

    if (has_bemf_sources_) {
        // PMSM netlist: the back-EMF is an in-circuit external source, so the
        // V-sources receive only the inverter terminal voltages and Veu/Vev/Vew
        // receive the phase back-EMFs.
        pending_vu_.store(static_cast<double>(va - vn));
        pending_vv_.store(static_cast<double>(vb - vn));
        pending_vw_.store(static_cast<double>(vc - vn));
        pending_eu_.store(static_cast<double>(ea));
        pending_ev_.store(static_cast<double>(eb));
        pending_ew_.store(static_cast<double>(ec));
    } else {
        // RL netlist: the back-EMF has to be folded into the V-source value
        // because the circuit contains no element for it.
        pending_vu_.store(static_cast<double>(va - vn - ea));
        pending_vv_.store(static_cast<double>(vb - vn - eb));
        pending_vw_.store(static_cast<double>(vc - vn - ec));
    }
}

float NgspicePlant::ReadCurrent(const char* vecname) const {
    if (!fn_ngGet_Vec_Info_) return 0.0f;
    pvector_info info = fn_ngGet_Vec_Info_(const_cast<char*>(vecname));
    if (!info || !info->v_realdata || info->v_length <= 0) return 0.0f;
    return static_cast<float>(info->v_realdata[info->v_length - 1]);
}

void NgspicePlant::IntegrateMechanics(float dt_s) {
    const float omega = state_.omega_e_rad_s;
    const float p = static_cast<float>(params_.pole_pairs);
    const float torque =
        1.5f * p *
        (params_.flux_wb * state_.iq_a +
         (params_.ld_h - params_.lq_h) * state_.id_a * state_.iq_a);
    const float friction = params_.friction_nm_per_rad_s * omega;
    const float domega = (torque - friction) / params_.inertia_kg_m2;

    state_.omega_e_rad_s += domega * dt_s;
    state_.theta_e_rad = WrapAngle(state_.theta_e_rad + omega * dt_s);
}

void NgspicePlant::Step(float duty_u_pct, float duty_v_pct, float duty_w_pct,
                        float dt_s) {
    if (!sharedspice_loaded_ || !circuit_loaded_ || analysis_failed_) return;
    if (dt_s <= 0.0f) return;

    UpdatePendingVoltages(duty_u_pct, duty_v_pct, duty_w_pct);

    // Advance the SPICE analysis in substeps_ chunks of the control period;
    // the phase voltages are held (zero-order hold) across the whole period.
    const double sub_dt = static_cast<double>(dt_s) / substeps_;
    const double t0 = current_sim_time_;
    for (int s = 1; s <= substeps_; ++s) {
        if (!AdvanceSpiceTo(t0 + sub_dt * s)) {
            if (!analysis_failed_) {
                analysis_failed_ = true;
                std::cerr << "HostSim: ngspice background analysis failed to "
                             "respond; plant state frozen\n";
            }
            return;
        }
    }
    current_sim_time_ = t0 + static_cast<double>(dt_s);

    const float ia = ReadCurrent("i(vu)");
    const float ib = ReadCurrent("i(vv)");
    const float ic = ReadCurrent("i(vw)");

    state_.ia_a = ia;
    state_.ib_a = ib;
    state_.ic_a = ic;

    const float theta = state_.theta_e_rad;
    const float cos_t = std::cos(theta);
    const float sin_t = std::sin(theta);

    const float i_alpha = ia;
    const float i_beta = (ia + 2.0f * ib) / kSqrt3;

    state_.id_a = i_alpha * cos_t + i_beta * sin_t;
    state_.iq_a = -i_alpha * sin_t + i_beta * cos_t;

    IntegrateMechanics(dt_s);
}

bool NgspicePlant::AdvanceSpiceTo(double target_time) {
    if (stop_active_) {
        Command("delete all");
        stop_active_ = false;
    }
    {
        std::ostringstream oss;
        oss << std::setprecision(12) << "stop when time > " << target_time;
        Command(oss.str().c_str());
        stop_active_ = true;
    }

    uint64_t target_pauses;
    {
        std::lock_guard<std::mutex> lk(bg_mutex_);
        target_pauses = bg_pauses_ + 1;
    }

    sync_target_time_.store(target_time);
    sync_redo_block_.store(false);

    if (first_step_) {
        Command("bg_run");
        first_step_ = false;
    } else {
        // Note: plain "resume" makes ngspice re-run the whole transient from
        // t=0; "bg_resume" continues the paused background analysis.
        Command("bg_resume");
    }

    return WaitForBgPause(target_pauses);
}

bool NgspicePlant::WaitForBgPause(uint64_t target_pauses) {
    std::unique_lock<std::mutex> lk(bg_mutex_);
    // The timeout is deliberately generous: a missed event freezes the plant
    // (logged once) instead of hanging the whole simulator.
    return bg_cv_.wait_for(lk, std::chrono::seconds(10),
                           [&] { return bg_pauses_ >= target_pauses; });
}

int NgspicePlant::CallbackSendChar(char* output, int /*ident*/,
                                    void* /*userdata*/) {
    if (!output) return 0;
    // ngspice routes all of its stdout through this callback. The bulk of it
    // is per-run/per-resume banner noise ("Doing analysis...", breakpoint
    // chatter), which would flood std::cerr and dominate the step cost, so
    // only genuine diagnostics are forwarded.
    std::string chunk(output);
    for (auto& c : chunk) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (chunk.find("error") != std::string::npos) {
        std::cerr << "[ngspice] " << output;
    }
    return 0;
}

int NgspicePlant::CallbackSendStat(char* /*output*/, int /*ident*/,
                                    void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackControlledExit(int /*exitstatus*/,
                                          bool /*immediate*/,
                                          bool /*quit*/, int /*ident*/,
                                          void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackSendData(pvecvaluesall /*data*/,
                                    int /*structcount*/, int /*ident*/,
                                    void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackSendInitData(pvecinfoall /*data*/, int /*ident*/,
                                        void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackBGThreadRunning(bool running, int /*ident*/,
                                           void* userdata) {
    auto* self = static_cast<NgspicePlant*>(userdata);
    if (!self) return 0;
    // Despite the parameter name, the sharedspice module delivers the
    // internal "not running" flag here (verified against ngspice-42):
    // true == background analysis paused/halted, false == started.
    if (running) {
        {
            std::lock_guard<std::mutex> lk(self->bg_mutex_);
            self->bg_running_ = false;
            ++self->bg_pauses_;
        }
        self->bg_cv_.notify_all();
    } else {
        std::lock_guard<std::mutex> lk(self->bg_mutex_);
        self->bg_running_ = true;
    }
    return 0;
}

int NgspicePlant::CallbackGetVSRCData(double* vval, double /*timeval*/,
                                       char* node, int /*ident*/,
                                       void* userdata) {
    if (!vval || !node || !userdata) return 1;
    auto* self = static_cast<NgspicePlant*>(userdata);

    if (CaseInsensitiveCompare(node, "u_node") == 0 ||
        CaseInsensitiveCompare(node, "vu") == 0) {
        *vval = self->pending_vu_.load();
    } else if (CaseInsensitiveCompare(node, "v_node") == 0 ||
               CaseInsensitiveCompare(node, "vv") == 0) {
        *vval = self->pending_vv_.load();
    } else if (CaseInsensitiveCompare(node, "w_node") == 0 ||
               CaseInsensitiveCompare(node, "vw") == 0) {
        *vval = self->pending_vw_.load();
    } else if (CaseInsensitiveCompare(node, "u_e") == 0 ||
               CaseInsensitiveCompare(node, "veu") == 0) {
        *vval = self->pending_eu_.load();
    } else if (CaseInsensitiveCompare(node, "v_e") == 0 ||
               CaseInsensitiveCompare(node, "vev") == 0) {
        *vval = self->pending_ev_.load();
    } else if (CaseInsensitiveCompare(node, "w_e") == 0 ||
               CaseInsensitiveCompare(node, "vew") == 0) {
        *vval = self->pending_ew_.load();
    } else {
        *vval = 0.0;
    }
    return 0;
}

int NgspicePlant::CallbackGetISRCData(double* ival, double /*timeval*/,
                                       char* /*node*/, int /*ident*/,
                                       void* /*userdata*/) {
    if (ival) *ival = 0.0;
    return 0;
}

int NgspicePlant::CallbackGetSyncData(double actualtime, double* deltatime,
                                       double /*olddelta*/, int redostep,
                                       int /*ident*/, int /*location*/,
                                       void* userdata) {
    if (!deltatime || !userdata) return 0;
    auto* self = static_cast<NgspicePlant*>(userdata);
    if (!self->sync_override_ || !self->circuit_loaded_) return 0;
    if (redostep != 0) {
        // ngspice is redoing a step (non-convergence / truncation error): let
        // its own reduced delta stand, and stop overriding for the rest of
        // this substep so we can never fight its error control (no livelock).
        self->sync_redo_block_.store(true);
        return 0;
    }
    if (self->sync_redo_block_.load()) return 0;
    // sharedspice's sharedsync() applies whatever the callback leaves in
    // *deltatime as the next CKTdelta (clamped only to the final time), so we
    // can replace the post-breakpoint re-ramp with a jump straight to the
    // current stop target. ngspice's own breakpoint truncation happens before
    // this callback, so assigning the remaining distance can neither overshoot
    // the stop breakpoint nor skip a pause.
    const double remaining = self->sync_target_time_.load() - actualtime;
    if (remaining > *deltatime) *deltatime = remaining;
    return 0;
}

} // namespace hostsim
