#include "ngspice_plant.h"

#include <algorithm>
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
#include <string_view>
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

// Packed free-run gate word: (actual-time double bits & ~1) | waiting flag.
uint64_t PackGate(double t, bool waiting) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(t), "gate packing needs 64-bit double");
    std::memcpy(&bits, &t, sizeof(bits));
    return (bits & ~1ULL) | (waiting ? 1ULL : 0ULL);
}

double GateTime(uint64_t gate) {
    const uint64_t bits = gate & ~1ULL;
    double t = 0.0;
    std::memcpy(&t, &bits, sizeof(t));
    return t;
}

#if defined(_WIN32)
// WaitOnAddress needs Windows 8+; the project's _WIN32_WINNT may hide the
// prototypes, so resolve them from kernel32 at load time and fall back to
// timed sleeps when unavailable.
using FnWaitOnAddress = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using FnWakeByAddressSingle = VOID(WINAPI*)(PVOID);

FnWaitOnAddress ResolveWaitOnAddress() {
    // Win11 24H2 stopped exporting these from kernel32.dll; the API-set
    // contract DLL has them on every supported Windows.
    const HMODULE m = GetModuleHandleA("api-ms-win-core-synch-l1-2-0.dll");
    return m ? reinterpret_cast<FnWaitOnAddress>(GetProcAddress(m, "WaitOnAddress"))
             : nullptr;
}

FnWakeByAddressSingle ResolveWakeByAddressSingle() {
    const HMODULE m = GetModuleHandleA("api-ms-win-core-synch-l1-2-0.dll");
    return m ? reinterpret_cast<FnWakeByAddressSingle>(
                   GetProcAddress(m, "WakeByAddressSingle"))
             : nullptr;
}

const FnWaitOnAddress g_wait_on_address = ResolveWaitOnAddress();
const FnWakeByAddressSingle g_wake_by_address_single = ResolveWakeByAddressSingle();

void WakeSingle(std::atomic<double>* address) {
    if (g_wake_by_address_single) g_wake_by_address_single(address);
}

void WakeSingle(std::atomic<uint64_t>* address) {
    if (g_wake_by_address_single) g_wake_by_address_single(address);
}
#endif

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
    debug_timing_ = std::getenv("HOSTSIM_NGSPICE_DEBUG") != nullptr;
#if defined(_WIN32)
    if (debug_timing_) {
        std::fprintf(stderr, "[ngspice-dbg] WaitOnAddress=%p Wake=%p\n",
                     reinterpret_cast<void*>(g_wait_on_address),
                     reinterpret_cast<void*>(g_wake_by_address_single));
    }
#endif
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

NgspicePlant::~NgspicePlant() {
    if (sharedspice_loaded_) {
        // Release any parked sync wait so the ngspice thread can observe the
        // halt, then give it a moment to stop before the library is unloaded
        // out from under it.
        shutdown_.store(true);
        RaiseAllowance(1e30);
        Command("bg_halt");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fn_ngSpice_running_ && fn_ngSpice_running_() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    UnloadSharedLibrary();
}

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

void NgspicePlant::RaiseAllowance(double t) {
    // Every allowance store must wake a parked sync callback; on Windows the
    // park loop blocks in WaitOnAddress (wake latency ~1-2 us, immune to the
    // 1-15 ms sleep_for timer quantization that previously dominated waits).
    sync_allow_time_.store(t, std::memory_order_release);
#if defined(_WIN32)
    WakeSingle(&sync_allow_time_);
#endif
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

    if (!sharedspice_loaded_) {
        first_step_ = true;
        return;
    }

    if (!circuit_loaded_) {
        LoadNetlist();
    }

    ApplyParams();

    if (circuit_loaded_ && !first_step_) {
        // The ngspice thread is parked in the sync callback; release it so the
        // halt can be processed, then wait for the run to actually stop
        // (ngspice rejects "reset" while its background thread is active).
        RaiseAllowance(1e30);
        Command("bg_halt");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fn_ngSpice_running_() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    first_step_ = true;
    sync_allow_time_.store(0.0);
    sync_actual_time_.store(0.0);
    sync_waiting_.store(false);
    sync_gate_.store(PackGate(0.0, false));
    ngspice_time_.store(0.0);

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

    pending_vu_.store(static_cast<double>(va - vn - ea));
    pending_vv_.store(static_cast<double>(vb - vn - eb));
    pending_vw_.store(static_cast<double>(vc - vn - ec));
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

    const int steps = std::max(1, substeps_);
    const float sub_dt = dt_s / static_cast<float>(steps);

    for (int i = 0; i < steps; ++i) {
        UpdatePendingVoltages(duty_u_pct, duty_v_pct, duty_w_pct);

        const double target_time = current_sim_time_ + static_cast<double>(sub_dt);
        target_time_.store(target_time);
        // ngspice free-runs and is throttled by CallbackGetSyncData at the
        // allowance boundary, so no bg_halt/bg_resume round-trip (~100 ms of
        // command dispatch latency per pause) is needed. Raise the allowance
        // only after the stimulus for this interval is in place. The raise
        // timestamp is stored first (release) so the park loop, having
        // observed the new allowance (acquire), never reads a stale timestamp.
        if (debug_timing_) {
            allow_raise_us_.store(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_release);
        }
        RaiseAllowance(target_time);

        if (first_step_) {
            Command("bg_run");
            first_step_ = false;
        }

        WaitUntilReached(target_time);
        // Track the actual simulation time ngspice reports, rather than the
        // requested target, to avoid drift.
        current_sim_time_ = sync_actual_time_.load();
    }

    // SPICE source current i(vu) is positive out of the source's + terminal
    // (toward ground), which is the negative of the current into the RL branch.
    const float ia = -ReadCurrent("i(vu)");
    const float ib = -ReadCurrent("i(vv)");
    const float ic = -ReadCurrent("i(vw)");

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

bool NgspicePlant::WaitUntilReached(double target_time) {
    constexpr auto kTimeout = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    const auto wait_start = std::chrono::steady_clock::now();
    bool slept = false;

    // Wait until the sync callback reports ngspice has reached the target
    // (SendData/ngspice_time_ lags one internal step behind) and has parked
    // the ngspice thread there, so vector reads below cannot race an
    // in-progress timestep. The gate word packs both conditions, so a single
    // load decides; after a short yield spin (the common fast case) block in
    // WaitOnAddress, which wakes in ~1-2 us instead of rounding up to the
    // 1-15.6 ms sleep_for timer granularity.
    bool saw_running = false;
    bool reached = false;
    for (int spins = 0; spins < 200 && !reached; ++spins) {
        const uint64_t gate = sync_gate_.load(std::memory_order_acquire);
        reached = GateTime(gate) >= target_time && (gate & 1ULL);
        if (!reached) std::this_thread::yield();
    }
    while (!reached) {
        slept = true;
        const uint64_t gate = sync_gate_.load(std::memory_order_acquire);
        if (GateTime(gate) >= target_time && (gate & 1ULL)) break;
        if (fn_ngSpice_running_()) {
            saw_running = true;
        } else if (saw_running && GateTime(gate) < target_time) {
            std::cerr << "HostSim: ngspice run ended early at t="
                      << GateTime(gate) << " s (target " << target_time
                      << " s), aborting step.\n";
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "HostSim: ngspice did not reach target time " << target_time
                      << " s within "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout).count()
                      << " ms (stuck at t=" << GateTime(gate)
                      << " s, ngspice_time=" << ngspice_time_.load() << " s), aborting step.\n";
            return false;
        }
#if defined(_WIN32)
        uint64_t observed = gate;
        // Sleeps only while the gate word still equals the just-checked value;
        // a store landing before the wait makes it return immediately, and the
        // callback wakes after every store, so no wake can be lost. The 50 ms
        // slice bounds how often the liveness checks above are re-evaluated.
        if (g_wait_on_address) {
            g_wait_on_address(&sync_gate_, &observed, sizeof(observed), 50);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    if (debug_timing_) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - wait_start)
                            .count();
        dbg_wait_us_.fetch_add(static_cast<uint64_t>(us));
        dbg_wait_count_.fetch_add(1);
        if (slept) dbg_wait_sleeps_.fetch_add(1);
        const uint64_t n = dbg_wait_count_.load();
        if (n % 20000 == 0) {
            const uint64_t wc = dbg_wake_count_.load();
            const uint64_t pc = dbg_park_count_.load();
            std::fprintf(stderr,
                         "[ngspice-dbg] waits=%llu avg_wait=%.1fus (sleeps %llu) "
                         "parks=%llu avg_park=%.1fus (sleeps %llu) avg_wake=%.1fus "
                         "sync_calls=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<double>(dbg_wait_us_.load()) / n,
                         static_cast<unsigned long long>(dbg_wait_sleeps_.load()),
                         static_cast<unsigned long long>(pc),
                         pc ? static_cast<double>(dbg_park_us_.load()) / pc : 0.0,
                         static_cast<unsigned long long>(dbg_park_sleeps_.load()),
                         wc ? static_cast<double>(dbg_wake_us_.load()) / wc : 0.0,
                         static_cast<unsigned long long>(dbg_sync_calls_.load()));
        }
    }
    return true;
}

int NgspicePlant::CallbackSendChar(char* output, int /*ident*/,
                                    void* /*userdata*/) {
    if (!output) return 0;

    // Suppress the repetitive halt/resume chatter that ngspice emits during
    // every Step() pause cycle.
    const std::string_view sv(output);
    if (sv.find("doAnalyses: pause requested") != std::string_view::npos ||
        sv.find("run simulation interrupted") != std::string_view::npos ||
        sv.find("simulation interrupted") != std::string_view::npos ||
        sv.find("Background thread stopped with timeout") != std::string_view::npos ||
        sv.find("Reference value") != std::string_view::npos ||
        sv.find("Doing analysis at TEMP") != std::string_view::npos) {
        return 0;
    }

    std::cerr << "[ngspice] " << output;
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

int NgspicePlant::CallbackSendData(pvecvaluesall data,
                                    int /*structcount*/, int /*ident*/,
                                    void* userdata) {
    if (!data || !userdata) return 0;
    auto* self = static_cast<NgspicePlant*>(userdata);

    // Capture the latest simulation time from the scale vector so the main
    // thread can decide when to call bg_halt.
    for (int i = 0; i < data->veccount; ++i) {
        pvecvalues v = data->vecsa[i];
        if (!v) continue;
        if (v->is_scale) {
            self->ngspice_time_.store(v->creal);
            break;
        }
    }
    return 0;
}

int NgspicePlant::CallbackSendInitData(pvecinfoall /*data*/, int /*ident*/,
                                        void* /*userdata*/) {
    return 0;
}

int NgspicePlant::CallbackBGThreadRunning(bool running, int /*ident*/,
                                           void* userdata) {
    if (userdata) {
        static_cast<NgspicePlant*>(userdata)->bg_running_cb_.store(running);
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

int NgspicePlant::CallbackGetSyncData(double actualtime,
                                       double* deltatime,
                                       double olddelta, int redostep,
                                       int /*ident*/, int /*location*/,
                                       void* userdata) {
    auto* self = static_cast<NgspicePlant*>(userdata);
    if (!self) return 0;
    if (self->shutdown_.load()) return 0;
    if (self->debug_timing_) {
        const uint64_t c = self->dbg_sync_calls_.fetch_add(1);
        if (c < 40) {
            std::fprintf(stderr,
                         "[ngspice-dbg] sync#%llu t=%.3g dt=%.3g olddt=%.3g redo=%d allow=%.3g\n",
                         static_cast<unsigned long long>(c), actualtime,
                         deltatime ? *deltatime : -1.0, olddelta, redostep,
                         self->sync_allow_time_.load());
        }
    }

    // Track the true circuit time: SendData (ngspice_time_) lags one internal
    // step behind because the boundary step blocks here before its data point
    // is emitted.
    self->sync_actual_time_.store(actualtime);

    // Throttle ngspice so it never runs ahead of the controller: Step() raises
    // sync_allow_time_ only after the stimulus (PWM duties) for that interval
    // has been computed. ngspice blocks here at the boundary, which replaces
    // bg_halt/bg_resume stepping and its ~100 ms per-pause dispatch latency.
    // The timeout keeps a wedged caller from hanging the ngspice thread
    // forever; running ahead briefly is the lesser evil.
    constexpr auto kTimeout = std::chrono::seconds(10);
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    const bool at_boundary = actualtime >= self->sync_allow_time_.load();
    // Publish time + parked state as one gate word (waiting flag is stored
    // first and packed into the same word, so WaitUntilReached can never
    // observe the new time without the flag) and wake any blocked waiter.
    self->sync_waiting_.store(at_boundary);
    self->sync_gate_.store(PackGate(actualtime, at_boundary),
                           std::memory_order_release);
#if defined(_WIN32)
    WakeSingle(&self->sync_gate_);
#endif
    if (at_boundary) {
        const auto park_start = std::chrono::steady_clock::now();
        bool slept = false;
        // Load the allowance once per wait round and block on exactly that
        // value: a raise landing between the condition check and the wait
        // makes WaitOnAddress return immediately (stored value != observed),
        // instead of sleeping out a whole 50 ms slice with the predicate
        // already satisfied.
        double observed = self->sync_allow_time_.load(std::memory_order_acquire);
        while (actualtime >= observed) {
            if (self->shutdown_.load()) {
                self->sync_waiting_.store(false);
                return 0;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                self->sync_waiting_.store(false);
                return 0;
            }
            slept = true;
#if defined(_WIN32)
            // Blocked park: ~1-2 us wake latency via WakeByAddressSingle in
            // RaiseAllowance, with none of the 1-15.6 ms sleep_for timer
            // quantization that used to dominate live-mode stepping. The
            // 50 ms slice only bounds shutdown/timeout re-checks.
            if (g_wait_on_address) {
                g_wait_on_address(&self->sync_allow_time_, &observed,
                                  sizeof(observed), 50);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
            observed = self->sync_allow_time_.load(std::memory_order_acquire);
        }
        if (self->debug_timing_) {
            const auto now = std::chrono::steady_clock::now();
            const auto park_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     now - park_start)
                                     .count();
            const int64_t raise_us = self->allow_raise_us_.load(std::memory_order_acquire);
            const int64_t wake_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch())
                    .count() -
                raise_us;
            self->dbg_park_us_.fetch_add(static_cast<uint64_t>(park_us));
            self->dbg_park_count_.fetch_add(1);
            if (slept) self->dbg_park_sleeps_.fetch_add(1);
            if (wake_us >= 0 && raise_us > 0) {
                self->dbg_wake_us_.fetch_add(static_cast<uint64_t>(wake_us));
                self->dbg_wake_count_.fetch_add(1);
            }
        }
        self->sync_waiting_.store(false);
    }

    // Clamp the next timestep so ngspice lands exactly on the allowed
    // boundary instead of stepping past it with stale stimulus.
    if (deltatime) {
        const double headroom = self->sync_allow_time_.load() - actualtime;
        if (headroom > 0.0 && *deltatime > headroom) {
            *deltatime = headroom;
        }
    }
    return 0;
}

} // namespace hostsim
