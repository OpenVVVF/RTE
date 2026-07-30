#include "ngspice_plant.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
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
    if (LoadSharedLibrary() && BindSymbols()) {
        sharedspice_loaded_ = true;
        int ident = 0;
        fn_ngSpice_Init_(CallbackSendChar, CallbackSendStat, CallbackControlledExit,
                 CallbackSendData, CallbackSendInitData, CallbackBGThreadRunning,
                 this);
        fn_ngSpice_Init_Sync_(CallbackGetVSRCData, CallbackGetISRCData,
                      CallbackGetSyncData, &ident, this);
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
    lib_handle_ = dlopen("libngspice.so", RTLD_NOW);
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

    if (!sharedspice_loaded_) return;

    if (!circuit_loaded_) {
        LoadNetlist();
    }

    ApplyParams();

    if (circuit_loaded_) {
        Command("reset");
    }
}

float NgspicePlant::ThetaElectricalDeg() const {
    return state_.theta_e_rad * 180.0f / kPi;
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

    pending_vu_ = static_cast<double>(va - vn - ea);
    pending_vv_ = static_cast<double>(vb - vn - eb);
    pending_vw_ = static_cast<double>(vc - vn - ec);
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
    if (!sharedspice_loaded_) return;
    if (dt_s <= 0.0f) return;

    UpdatePendingVoltages(duty_u_pct, duty_v_pct, duty_w_pct);

    const double target_time = current_sim_time_ + static_cast<double>(dt_s);

    // Use an interactive stop condition to pause the background thread at
    // the next HostSim time boundary. ngSpice_SetBkpt is kept available for
    // callers that want it, but the "stop when" command is more reliable
    // across ngspice versions for this synchronous stepping pattern.
    Command("delete");
    {
        std::ostringstream oss;
        oss << std::setprecision(12) << "stop when time > " << target_time;
        Command(oss.str().c_str());
    }

    if (first_step_) {
        Command("bg_run");
        first_step_ = false;
    } else {
        Command("resume");
    }

    // Wait for the background thread to start, then wait for it to pause.
    constexpr int kMaxStartPolls = 10000;
    int polls = 0;
    while (!fn_ngSpice_running_() && polls < kMaxStartPolls) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        ++polls;
    }
    while (fn_ngSpice_running_()) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

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

    current_sim_time_ = target_time;
}

int NgspicePlant::CallbackSendChar(char* output, int /*ident*/,
                                    void* /*userdata*/) {
    if (output) {
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

int NgspicePlant::CallbackBGThreadRunning(bool /*running*/, int /*ident*/,
                                           void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackGetVSRCData(double* vval, double /*timeval*/,
                                       char* node, int /*ident*/,
                                       void* userdata) {
    if (!vval || !node || !userdata) return 1;
    auto* self = static_cast<NgspicePlant*>(userdata);

    if (CaseInsensitiveCompare(node, "u_node") == 0 ||
        CaseInsensitiveCompare(node, "vu") == 0) {
        *vval = self->pending_vu_;
    } else if (CaseInsensitiveCompare(node, "v_node") == 0 ||
               CaseInsensitiveCompare(node, "vv") == 0) {
        *vval = self->pending_vv_;
    } else if (CaseInsensitiveCompare(node, "w_node") == 0 ||
               CaseInsensitiveCompare(node, "vw") == 0) {
        *vval = self->pending_vw_;
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

int NgspicePlant::CallbackGetSyncData(double /*actualtime*/,
                                       double* /*deltatime*/,
                                       double /*olddelta*/, int /*redostep*/,
                                       int /*ident*/, int /*location*/,
                                       void* /*userdata*/) {
    return 0;
}

} // namespace hostsim
