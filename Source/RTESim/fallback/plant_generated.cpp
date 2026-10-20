/**
 * Fallback plant implementation. Field-for-field port of the proven
 * monolithic twin plant (twin/native/src/plant_pmsm.cpp) behind the
 * PlantCodegen contract; used only when PlantCodegenLib is not in the build.
 *
 * One PlantStep(dt) == one Euler substep of twin::PlantPmsm::step()'s inner
 * loop (the monolith runs dt_sub=200us twice per 400us control period; the
 * rte-sim scheduler calls PlantStep the same way and collects measurements
 * once per period, identical cadence).
 *
 * Constants: twin/params/gen7_size2.json + machine zf75_10_pmsm including
 * the 2026-10-20 calibration; supply defaults match the gen7_bench_pmsm
 * plant graph's DcBus node (144.7 V / 400 A / sink 50 A / 5 mOhm).
 */

#include "plant_generated.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace plant {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPiThird = 2.0 * kPi / 3.0;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kSqrt3 = 1.7320508075688772;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
} // namespace

void PlantInit(PlantState& s) {
    s = PlantState{};          // re-paste defaults (gen7Size2 in the monolith)
    s.v_dc = s.supply_v_set;   // monolith: plant.v_dc = supply.v_set at boot
    s.t_base_c = s.t_amb;
}

void PlantSetDuty(PlantState& s, float du, float dv, float dw) {
    s.duty_u = du;
    s.duty_v = dv;
    s.duty_w = dw;
}

void PlantSetDynoRpm(PlantState& s, float rpm) {
    s.dyno_rpm = rpm;
}

void PlantSetFault(PlantState& s, const char* name, bool active) {
    if (!name) return;
    if (!std::strcmp(name, "OpenGates")) s.open_gates = active;
    else if (!std::strcmp(name, "EncoderDisconnect")) s.encoder_disconnect = active;
    else if (!std::strcmp(name, "SensorRail")) s.sensor_rail = active;
}

bool PlantSetInput(PlantState& s, const char* name, float value) {
    if (!name) return false;
    if (!std::strcmp(name, "OpenGates")) { s.open_gates = value >= 0.5f; return true; }
    if (!std::strcmp(name, "EncoderDisconnect")) { s.encoder_disconnect = value >= 0.5f; return true; }
    if (!std::strcmp(name, "SensorRail")) { s.sensor_rail = value >= 0.5f; return true; }
    return false;
}

void PlantSetSupply(PlantState& s, double v_set, double i_limit_a,
                    double sink_limit_a, bool can_sink) {
    s.supply_v_set = v_set;
    s.supply_i_limit_a = i_limit_a;
    s.supply_sink_limit_a = sink_limit_a;
    s.supply_can_sink = can_sink;
}

void PlantStep(PlantState& s, double dt) {
    const double h = dt;   // one substep == monolith inner-loop body

    // dyno mode: shaft speed is host-held (plant.Shaft.Dyno semantics)
    s.omega_e = s.dyno_rpm * kTwoPi / 60.0 * s.pp;

    const bool gates = !s.open_gates;
    double vu_raw = 0.0, vv_raw = 0.0, vw_raw = 0.0;
    if (gates) {
        // leg switching node -> phase-to-neutral-mid: (d - d_avg)/50 * vdc
        // reproduces the firmware SVPWM convention (duty_pct = 50 + 50*(v-v_off))
        const double d_avg = (s.duty_u + s.duty_v + s.duty_w) / 3.0;
        vu_raw = (s.duty_u - d_avg) / 50.0 * s.v_dc;
        vv_raw = (s.duty_v - d_avg) / 50.0 * s.v_dc;
        vw_raw = (s.duty_w - d_avg) / 50.0 * s.v_dc;
    }
    const double td_overhead = s.dt_dead * s.f_sw;   // deadtime distortion factor

    const double ct = std::cos(s.theta_e);
    const double st = std::sin(s.theta_e);
    // plant truth -> abc currents (inverse Park, peak convention)
    s.ia = ct * s.id_a - st * s.iq_a;
    s.ib = std::cos(s.theta_e - kTwoPiThird) * s.id_a - std::sin(s.theta_e - kTwoPiThird) * s.iq_a;
    s.ic = -s.ia - s.ib;

    auto phase = [&](double v_cmd, double i_p) {
        if (!gates) return 0.0;
        if (std::fabs(i_p) < 1e-3) return v_cmd;
        const double s_i = i_p > 0.0 ? 1.0 : -1.0;
        return v_cmd - s_i * (s.v_ce0 + td_overhead * s.v_dc + s.r_ce * std::fabs(i_p));
    };
    const double vu = phase(vu_raw, s.ia);
    const double vv = phase(vv_raw, s.ib);
    const double vw = phase(vw_raw, s.ic);

    // Clarke (power-invariant pair) + Park -> applied dq
    const double valpha = (2.0 / 3.0) * (vu - 0.5 * (vv + vw));
    const double vbeta = (2.0 / 3.0) * (kSqrt3 / 2.0) * (vv - vw);
    const double vd = ct * valpha + st * vbeta;
    const double vq = -st * valpha + ct * vbeta;

    const double did = (vd - s.rs * s.id_a + s.omega_e * s.lq * s.iq_a) / s.ld;
    const double diq = (vq - s.rs * s.iq_a - s.omega_e * (s.ld * s.id_a + s.lam)) / s.lq;
    s.id_a += did * h;
    s.iq_a += diq * h;
    s.theta_e = std::fmod(s.theta_e + s.omega_e * h, kTwoPi);
    if (s.theta_e < 0.0) s.theta_e += kTwoPi;

    const double te = 1.5 * s.pp * (s.lam * s.iq_a + (s.ld - s.lq) * s.id_a * s.iq_a);
    // (dyno holds the shaft; the FreeInertia branch of the monolith is not
    // part of the bench fallback)

    // losses from the fitted phenomenological law (energy side; the
    // per-phase knee/deadtime terms above are voltage shaping only)
    const double im = std::hypot(s.id_a, s.iq_a);
    const double om = std::fabs(s.omega_e) / s.pp;
    const double l_total = gates ? (s.loss_k1 * im + s.loss_k2 * im * im + s.loss_k3 * om
                                    + s.loss_k4 * im * om + s.loss_k5 * s.v_dc * im)
                                 : 0.0;
    const double p_shaft = 1.5 * (vd * s.id_a + vq * s.iq_a);
    const double om_m = s.omega_e / s.pp;
    const double p_em = gates ? te * om_m : 0.0;
    // link-side energy: airgap power +/- total fitted losses
    const double p_link_drawn = gates ? (p_em + l_total) : p_shaft;

    // DC bus with bench-supply model (positive dclink convention = into the link)
    const double r_s = std::max(s.supply_series_r_mohm / 1e3, 1e-9);
    double i_sup = (s.supply_v_set - s.v_dc) / r_s;
    const double lo = s.supply_can_sink ? -s.supply_sink_limit_a : 0.0;
    const double hi = s.supply_i_limit_a;
    if (s.supply_has_cv_clamp && s.v_dc > s.supply_cv_clamp_v) {
        i_sup = -std::min((s.v_dc - s.supply_cv_clamp_v) / r_s, s.supply_cv_clamp_sink_a);
    } else {
        i_sup = std::max(lo, std::min(i_sup, hi));
    }
    const double p_dc = p_link_drawn;
    const double i_inv = p_dc / std::max(s.v_dc, 1.0);
    s.v_dc += (i_sup - i_inv) / s.c_link * h;
    s.v_dc = std::min(std::max(s.v_dc, 0.5), 2000.0);   // physical rails; keep solver sane

    // thermal lump: plate share of the fitted loss + idle draw
    const double p_heat = s.plate_share * l_total + (gates ? s.p_idle_w : 0.0);
    s.t_base_c += (p_heat - (s.t_base_c - s.t_amb) / s.rth_sa) / s.cth_base * h;

    s.diag_p_dc = p_dc;
    s.diag_p_loss = l_total;
    s.diag_p_heat = p_heat;
    s.diag_te_nm = te;
    s.diag_p_shaft_em = p_em;
}

void PlantCollectMeas(const PlantState& s, PlantMeas& m) {
    // sin/cos encoder: one cycle per MECHANICAL rev, like the real encoder.
    // int() truncation toward zero, matching the python reference bit-exactly.
    if (s.encoder_disconnect) {
        m.Enc_Sin = 0.0;
        m.Enc_Cos = 0.0;
    } else {
        const double phi = (s.theta_e - s.enc_offset_true_deg * kPi / 180.0) / s.pp;
        m.Enc_Sin = static_cast<int>(s.enc_mid_sin + s.enc_amp_sin * std::sin(phi));
        m.Enc_Cos = static_cast<int>(s.enc_mid_cos + s.enc_amp_cos * std::cos(phi));
    }

    // hall quantization (offsets/noise are zero-channels in v0, as in plant.py);
    // SensorRail: isolated rail collapsed -> phantom rail (sign of offset,
    // 0.98x range, per Plant.Sensor.Hall/inline.cpp)
    auto hall = [&](double i_true) {
        if (s.sensor_rail) return 0.98 * s.i_range;   // Offset_A >= 0 branch
        const double q = std::round(i_true / s.i_lsb) * s.i_lsb;
        return std::max(-s.i_range, std::min(s.i_range, q));
    };

    const double omega_m = s.omega_e / s.pp;
    const double vdc = s.v_dc;
    const double p_dc = s.diag_p_dc;

    m.Iu_A = hall(s.ia);
    m.Iv_A = hall(s.ib);
    m.Iw_A = hall(s.ic);
    m.Vdc_V = std::round(vdc / s.vdc_lsb) * s.vdc_lsb;
    // positive dclink convention = out of the link (twin/native meas sign)
    m.Dclink_I_A = -p_dc / std::max(vdc, 1.0);
    m.Dclink_P_W = -p_dc;
    m.Mech_Rpm = omega_m * 60.0 / kTwoPi;
    m.Elec_Rpm = s.omega_e * 60.0 / kTwoPi;
    m.Temp_Inv1_C = s.t_base_c + s.temp_off1;
    m.Temp_Inv2_C = s.t_base_c + s.temp_off2_idle
        - s.temp_off2_load_slope * std::max(0.0, s.t_base_c - (s.t_amb + 4.9));
    m.Temp_Inv3_C = kNan;   // TS3 disabled on the bench (null channel)
    m.Temp_Motor_C = kNan;  // motor NTC not fitted (null channel)

    // MAX22530 phase-voltage channels: not modeled by the averaged plant
    m.Vu_V = 0.0;
    m.Vv_V = 0.0;
    m.Vw_V = 0.0;

    // truth-side power diagnostics for the tw_p_* telemetry keys
    m.P_Heat_W = s.diag_p_heat;
    m.P_Dc_W = s.diag_p_dc;
    m.P_ShaftEm_W = s.diag_p_shaft_em;
}

} // namespace plant
