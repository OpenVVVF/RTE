#pragma once

/**
 * Fallback plant honoring the PlantCodegen host contract
 * (namespace plant; PlantState/PlantInit/PlantStep/PlantCollectMeas/
 * PlantSetDuty/PlantSetDynoRpm/PlantSetFault/PlantSetInput/PlantSetSupply/
 * PlantSupplyDefaults/PlantBootParams — v1.1 surface).
 *
 * Used by SimRunner ONLY when this build has no PlantCodegenLib
 * (RTESIM_HAVE_PLANTCODEGEN undefined): the runner stages this file pair
 * into the scenario work dir under the name plant_generated.h/.cpp so the
 * scheduler and shim compile and behave identically in both configurations.
 *
 * Physics is a field-for-field port of the proven monolithic plant
 * twin/native/src/plant_pmsm.{h,cpp} (constants cited below), parameterized
 * to match RTE/Assets/PlantExamples/gen7_bench_pmsm.json (Gen7 'Size 2'
 * bench: ZF75-10 PMSM held by the dyno, CM600DY knee inverter, 19.8 mF DC
 * link, bench CV supply, Gen6FW sensor views).
 */

#include <limits>

namespace plant {

struct PlantMeas {
    // contract channels (twin/plantgraph MeasOut inputs)
    double Iu_A = 0.0, Iv_A = 0.0, Iw_A = 0.0;
    double Vdc_V = 0.0;
    double Enc_Sin = 0.0, Enc_Cos = 0.0;
    double Temp_Inv1_C = 0.0, Temp_Inv2_C = 0.0, Temp_Inv3_C = 0.0;
    double Temp_Motor_C = std::numeric_limits<double>::quiet_NaN();  // NTC not fitted
    double Mech_Rpm = 0.0, Elec_Rpm = 0.0;
    double Dclink_I_A = 0.0, Dclink_P_W = 0.0;
    double Vu_V = 0.0, Vv_V = 0.0, Vw_V = 0.0;

    // truth-side extras (PowerOut-side diagnostics, off the MeasOut view;
    // the scheduler logs them as tw_p_heat_w/tw_p_dc_w/tw_p_shaft_em_w)
    double P_Heat_W = 0.0, P_Dc_W = 0.0, P_ShaftEm_W = 0.0;
};

struct PlantState {
    // per-control-period externals (host-assigned)
    double duty_u = 50.0, duty_v = 50.0, duty_w = 50.0;   // percent, 50 = zero vector
    double dyno_rpm = 0.0;                                 // mechanical rpm (dyno-held)

    // electrical state (dq, peak)
    double id_a = 0.0, iq_a = 0.0;
    // mechanical
    double omega_e = 0.0;           // elec rad/s
    double theta_e = 0.0;           // elec rad, plant truth
    // bus
    double v_dc = 144.7;
    // thermal lump
    double t_base_c = 22.5;
    // last-substep abc currents (for PlantCollectMeas)
    double ia = 0.0, ib = 0.0, ic = 0.0;

    // faults (Plant.Fault.Point names)
    bool open_gates = false;         // OpenGates: gates die -> freewheel collapse
    bool encoder_disconnect = false; // EncoderDisconnect: counts rail to 0
    bool sensor_rail = false;        // SensorRail: isolated hall rail phantom

    // bench supply cfg (defaults = gen7_bench_pmsm.json DcBus parameters)
    double supply_v_set = 144.7;
    double supply_i_limit_a = 400.0;
    bool supply_can_sink = true;
    double supply_sink_limit_a = 50.0;
    double supply_series_r_mohm = 5.0;
    bool supply_has_cv_clamp = false;
    double supply_cv_clamp_v = 0.0;
    double supply_cv_clamp_sink_a = 0.0;

    // diagnostics carried between PlantStep and PlantCollectMeas
    double diag_p_dc = 0.0, diag_p_heat = 0.0, diag_p_shaft_em = 0.0;
    double diag_p_loss = 0.0, diag_te_nm = 0.0, diag_loss_law_floor_hits = 0.0;

    // ----- params (twin/native/src/plant_pmsm.h, pack gen7_size2 /
    //       machine zf75_10_pmsm incl. 2026-10-20 calibration) -----
    double rs = 0.00765;            // r_phase_ohm
    double ld = 67e-6;              // ld_h
    double lq = 125e-6;             // lq_h
    double pp = 5.0;                // pole_pairs
    double j = 0.05;                // j_kgm2 (gap fallback)
    double b = 1e-4;                // b_nms (gap fallback)
    double enc_offset_true_deg = 33.034;
    double enc_sign = 1.0;
    double lam = 0.02323;           // calibrated flux
    // fitted phenomenological loss law (loss_law v2, 400 A hold anchor)
    double loss_k1 = 4.12812;
    double loss_k2 = -0.00375376;
    double loss_k3 = -6.23468;
    double loss_k4 = 0.0378668;
    double loss_k5 = -0.000698642;
    double plate_share = 0.70;
    // power stage (CM600DY knee + deadtime)
    double v_ce0 = 1.15;
    double r_ce = 1.3e-3;
    double dt_dead = 1.0e-6;
    double f_sw = 5000.0;
    // sensing
    double i_lsb = 0.07;
    double i_range = 1200.0;
    double vdc_lsb = 0.44;
    // dc link
    double c_link = 19.8e-3;
    // thermal net (pasted interface assembly, fitted 2026-10-20)
    double rth_sa = 0.041;
    double cth_base = 9000.0;
    double p_idle_w = 120.0;
    double temp_off1 = 0.3;
    double temp_off2_idle = 3.4;
    double temp_off2_load_slope = 0.268;
    double t_amb = 22.5;
    // encoder count bounds (Encoder-Loss report healthy learned bounds)
    double enc_mid_sin = (11423.0 + 53864.0) / 2.0;
    double enc_amp_sin = (53864.0 - 11423.0) / 2.0;
    double enc_mid_cos = (11346.0 + 53727.0) / 2.0;
    double enc_amp_cos = (53727.0 - 11346.0) / 2.0;
};

void PlantInit(PlantState& state);
void PlantStep(PlantState& state, double dt);
void PlantCollectMeas(const PlantState& state, PlantMeas& meas);
void PlantSetDuty(PlantState& state, float du, float dv, float dw);
void PlantSetDynoRpm(PlantState& state, float rpm);
bool PlantSetFault(PlantState& state, const char* name, bool active);
bool PlantSetInput(PlantState& state, const char* name, float value);
void PlantSetSupply(PlantState& state, double v_set, double i_limit_a,
                    double sink_limit_a, bool can_sink, double cv_clamp_v,
                    double cv_clamp_sink_a, double series_r_mohm);
void PlantSupplyDefaults(const PlantState& state, double* v_set, double* i_limit_a,
                         double* sink_limit_a, bool* can_sink, double* cv_clamp_v,
                         double* cv_clamp_sink_a, double* series_r_mohm);
void PlantBootParams(const PlantState& state, double* poles, double* rs_ohm,
                     double* enc_offset_deg);

} // namespace plant
