#pragma once

#include "plant_backend.h"
#include "sharedspice.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hostsim {

/* How the plant interprets duties and read-back:
 * Motor: 3-phase inverter semantics — neutral-point subtraction, back-EMF
 *        handling, i(vu)/i(vv)/i(vw) phase currents, dq transform + PMSM
 *        mechanics in C++.
 * Dcdc:  3 independent legs — leg switch-node voltage is duty*VDC directly
 *        (no neutral subtraction, no back-EMF, no mechanics); leg currents
 *        read from the netlist's Vsen1..3 zero-volt sense sources, bus
 *        voltages probed from nodes bus1..3. */
enum class NgspicePlantMode { Motor = 0, Dcdc };

class NgspicePlant : public IPlant {
public:
    /* Per-step probes, meaningful in Dcdc mode only: bus voltages v(bus1..3)
     * and leg currents i(vsen1..3) sampled at the end of the last Step(). */
    struct DcdcProbes {
        float v_bus[3] = {0.0f, 0.0f, 0.0f};
        float i_leg[3] = {0.0f, 0.0f, 0.0f};
    };

    NgspicePlant();
    ~NgspicePlant() override;

    void SetParams(const MotorParams& params) override;
    void Reset() override;
    void Step(float duty_u_pct, float duty_v_pct, float duty_w_pct,
              float dt_s) override;

    const MotorState& State() const override { return state_; }
    float ThetaElectricalDeg() const override;
    float OmegaElectricalRadPerSec() const override {
        return state_.omega_e_rad_s;
    }

    void SetNetlistPath(const std::string& path) { netlist_path_ = path; }
    void SetSubsteps(int substeps) { substeps_ = (substeps > 0) ? substeps : 1; }
    void SetMode(NgspicePlantMode mode) { mode_ = mode; }
    NgspicePlantMode GetMode() const { return mode_; }
    /* Sim-time horizon the plant must cover, handed over before the first
     * Reset() (i.e. before the netlist is parsed). LoadNetlist raises a
     * .tran TSTOP shorter than this so the analysis cannot complete early;
     * live mode is unbounded and gets a stand-in horizon. */
    void SetPlannedDuration(double duration_s, bool live) {
        planned_duration_s_ = duration_s;
        live_mode_ = live;
    }

    /* Valid after the netlist has loaded (first Reset()). */
    bool NetlistIsDcdc() const { return netlist_is_dcdc_; }
    bool CircuitLoaded() const { return circuit_loaded_; }
    /* TSTOP parsed from the netlist's .tran card (post auto-raise), seconds;
     * < 0 when unknown (no .tran card or an unparseable value). */
    double NetlistTstopSeconds() const { return netlist_tstop_s_; }
    /* dcdc mode is only live when the netlist matches it; ModeMatchesNetlist
     * is the runtime's cue to fall back to another backend loudly. Motor mode
     * additionally requires the Vu/Vv/Vw external-source contract — without
     * them GetVSRCData never fires and the plant would run on silent zeros. */
    bool ModeMatchesNetlist() const {
        if (netlist_is_dcdc_ != (mode_ == NgspicePlantMode::Dcdc)) return false;
        if (mode_ == NgspicePlantMode::Motor && !has_motor_vsources_) {
            return false;
        }
        return true;
    }
    bool DcdcActive() const {
        return mode_ == NgspicePlantMode::Dcdc && netlist_is_dcdc_;
    }
    /* Last Step()'s probes; false when dcdc mode is not live. */
    bool GetDcdcProbes(DcdcProbes* out) const {
        if (!DcdcActive() || !out) return false;
        *out = probes_;
        return true;
    }

    bool IsSharedspiceLoaded() const { return sharedspice_loaded_; }

private:
    MotorParams params_{};
    MotorState state_{};
    std::string netlist_path_{};
    int substeps_ = 4;
    bool sharedspice_loaded_ = false;
    /* ngSpice_Init has run: the library may own a live worker thread, so the
     * shared library must never be dlclose'd again (deliberate one-time leak
     * instead of a crash while g_runtime tears down statically). */
    bool spice_initialized_ = false;
    bool circuit_loaded_ = false;
    // True when the loaded netlist exposes Veu/Vev/Vew external sources,
    // meaning the back-EMF is in-circuit and the V-sources take only the
    // inverter terminal voltages.
    bool has_bemf_sources_ = false;
    // True when the loaded netlist exposes Vu/Vv/Vw external sources — the
    // motor-mode drive contract (dcdc netlists expose them too).
    bool has_motor_vsources_ = false;
    // True when the loaded netlist exposes the Vsen1/Vsen2/Vsen3 sense-source
    // trio — the dcdc netlist contract marker (see plants/dcdc_buck.cir).
    bool netlist_is_dcdc_ = false;
    NgspicePlantMode mode_ = NgspicePlantMode::Motor;
    DcdcProbes probes_{};
    bool first_step_ = true;
    double current_sim_time_ = 0.0;
    // See SetPlannedDuration / NetlistTstopSeconds.
    double planned_duration_s_ = 0.0;
    bool live_mode_ = false;
    double netlist_tstop_s_ = -1.0;
    bool saliency_note_printed_ = false;

    // Read by ngspice's analysis thread via GetVSRCData; written by the host
    // thread in Step(). Atomics keep that cross-thread handoff safe.
    std::atomic<double> pending_vu_{0.0};
    std::atomic<double> pending_vv_{0.0};
    std::atomic<double> pending_vw_{0.0};
    // Back-EMF values for netlists that expose Veu/Vev/Vew external sources
    // (back-EMF in-circuit); unused otherwise.
    std::atomic<double> pending_eu_{0.0};
    std::atomic<double> pending_ev_{0.0};
    std::atomic<double> pending_ew_{0.0};

    // Trace-time target of the in-flight bg_run/bg_resume. The GetSyncData
    // callback reads it to override ngspice's post-breakpoint timestep cut
    // with a single "whole remaining substep" step (skip the re-ramp).
    std::atomic<double> sync_target_time_{0.0};
    // Runtime opt-in (HOSTSIM_NGSPICE_SYNC_JUMP=1): GetSyncData overrides the
    // post-breakpoint timestep cut and jumps straight to the stop target,
    // skipping ngspice's per-resume delta re-ramp (~1.8x fewer internal
    // steps, but only ~4% wall time on the RL/PMSM demos and slightly
    // different trajectories). Default off: identical behavior to not
    // having the override at all.
    bool sync_override_ = false;
    // Set by the sync callback whenever ngspice has to redo a step; cleared by
    // AdvanceSpiceTo when a new stop target is armed. While set, the callback
    // leaves the timestep to ngspice for the rest of that substep.
    std::atomic<bool> sync_redo_block_{false};

    void* lib_handle_ = nullptr;

    // BGThreadRunning reports each pause of the background analysis; the host
    // thread blocks on this instead of spin-polling ngSpice_running().
    std::mutex bg_mutex_;
    std::condition_variable bg_cv_;
    bool bg_running_ = false;
    uint64_t bg_pauses_ = 0;
    bool stop_active_ = false;
    /* Written by the ngspice analysis thread (CallbackControlledExit) and the
     * host thread alike. */
    std::atomic<bool> analysis_failed_{false};

    /* Log-once sets for external-source names the callbacks don't recognize
     * (called from the analysis thread; host thread never touches them). */
    std::mutex unknown_srcs_mu_;
    std::set<std::string> logged_unknown_srcs_;

    using FN_ngSpice_Init =
        int (*)(SendChar*, SendStat*, ControlledExit*, SendData*,
                SendInitData*, BGThreadRunning*, void*);
    using FN_ngSpice_Init_Sync =
        int (*)(GetVSRCData*, GetISRCData*, GetSyncData*, int*, void*);
    using FN_ngSpice_Command = int (*)(char*);
    using FN_ngSpice_Circ = int (*)(char**);
    using FN_ngSpice_running = bool (*)(void);
    using FN_ngSpice_CurPlot = char* (*)(void);
    using FN_ngSpice_AllPlots = char** (*)(void);
    using FN_ngSpice_AllVecs = char** (*)(char*);
    using FN_ngSpice_SetBkpt = bool (*)(double);
    using FN_ngGet_Vec_Info = pvector_info (*)(char*);

    FN_ngSpice_Init fn_ngSpice_Init_ = nullptr;
    FN_ngSpice_Init_Sync fn_ngSpice_Init_Sync_ = nullptr;
    FN_ngSpice_Command fn_ngSpice_Command_ = nullptr;
    FN_ngSpice_Circ fn_ngSpice_Circ_ = nullptr;
    FN_ngSpice_running fn_ngSpice_running_ = nullptr;
    FN_ngSpice_CurPlot fn_ngSpice_CurPlot_ = nullptr;
    FN_ngSpice_AllPlots fn_ngSpice_AllPlots_ = nullptr;
    FN_ngSpice_AllVecs fn_ngSpice_AllVecs_ = nullptr;
    FN_ngSpice_SetBkpt fn_ngSpice_SetBkpt_ = nullptr;
    FN_ngGet_Vec_Info fn_ngGet_Vec_Info_ = nullptr;

    static int CallbackSendChar(char* output, int ident, void* userdata);
    static int CallbackSendStat(char* output, int ident, void* userdata);
    static int CallbackControlledExit(int exitstatus, bool immediate,
                                       bool quit, int ident, void* userdata);
    static int CallbackSendData(pvecvaluesall data, int structcount,
                                 int ident, void* userdata);
    static int CallbackSendInitData(pvecinfoall data, int ident,
                                     void* userdata);
    static int CallbackBGThreadRunning(bool running, int ident,
                                        void* userdata);
    static int CallbackGetVSRCData(double* vval, double timeval, char* node,
                                    int ident, void* userdata);
    static int CallbackGetISRCData(double* ival, double timeval, char* node,
                                    int ident, void* userdata);
    static int CallbackGetSyncData(double actualtime, double* deltatime,
                                    double olddelta, int redostep, int ident,
                                    int location, void* userdata);

    bool LoadSharedLibrary();
    bool BindSymbols();
    void UnloadSharedLibrary();
    void Command(const char* cmd);
    void LoadNetlist();
    static bool DetectBackEmfSources(const std::vector<std::string>& lines);
    static bool DetectDcdcSenseSources(const std::vector<std::string>& lines);
    static bool DetectMotorVoltageSources(
        const std::vector<std::string>& lines);
    void RaiseTranTstopForRun(std::vector<std::string>* lines);
    void ApplyParams();
    void UpdatePendingVoltages(float du_pct, float dv_pct, float dw_pct);
    bool AdvanceSpiceTo(double target_time);
    bool WaitForBgPause(uint64_t target_pauses);
    /* Last sample of a tran vector; false when the vector is unknown/empty —
     * callers must not treat 0 as a reading in that case. */
    bool ReadVecLast(const char* vecname, double* out) const;
    float ReadVecLastF(const char* vecname) const;
    void LogUnknownSourceOnce(const char* node, bool is_voltage);
    void IntegrateMechanics(float dt_s);
    void StepDcdc(float duty_u_pct, float duty_v_pct, float duty_w_pct,
                  float dt_s);
};

} // namespace hostsim
