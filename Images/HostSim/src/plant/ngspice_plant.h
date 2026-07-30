#pragma once

#include "plant_backend.h"
#include "sharedspice.h"

#include <string>

namespace hostsim {

class NgspicePlant : public IPlant {
public:
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

    bool IsSharedspiceLoaded() const { return sharedspice_loaded_; }

private:
    MotorParams params_{};
    MotorState state_{};
    std::string netlist_path_{};
    int substeps_ = 4;
    bool sharedspice_loaded_ = false;
    bool circuit_loaded_ = false;
    bool first_step_ = true;
    double current_sim_time_ = 0.0;

    double pending_vu_ = 0.0;
    double pending_vv_ = 0.0;
    double pending_vw_ = 0.0;

    void* lib_handle_ = nullptr;

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
    void ApplyParams();
    void UpdatePendingVoltages(float du_pct, float dv_pct, float dw_pct);
    float ReadCurrent(const char* vecname) const;
    void IntegrateMechanics(float dt_s);
};

} // namespace hostsim
