/* Hybrid ADRC + predictive current control for Gen6 PMSM (55 V demo).
 *
 * Mode-3 deadbeat + Ki → SVPWM, mild residual ESO.
 * Optional outer speed loop: SpdMode>0.5 ⇒ I_Q_Ref is mechanical rpm setpoint.
 *
 * Tuned for clean no-load spin under a ~55 V bus (voltage ceiling ~650–750 rpm
 * with ψ≈0.07). Soft field-weakening + Iq-priority under modulation limit
 * reduce the sync-loss blowups seen in HybridADRCMPC-5/6.
 *
 * AXISRAM (.dma_buffers) is NOLOAD — magic-clear before use.
 */

struct HyAdrcMpccState {
    uint32_t magic;
    float u_alpha_prev;
    float u_beta_prev;
    float enable_s;
    float id_f;
    float iq_f;
    float z1_d;
    float z2_d;
    float z1_q;
    float z2_q;
    float id_int;
    float iq_int;
    float theta_prev;
    float omega_f;
    float spd_int;
    float iq_cmd_f;
    uint8_t filt_init;
    uint8_t eso_init;
    uint8_t omega_init;
    uint8_t _pad;
};

static constexpr uint32_t kHyMagic = 0x48594135u; /* HYA5 */

static HyAdrcMpccState hy __attribute__((section(".dma_buffers"), aligned(4)));

if (hy.magic != kHyMagic) {
    hy.u_alpha_prev = 0.0f;
    hy.u_beta_prev = 0.0f;
    hy.enable_s = 0.0f;
    hy.id_f = 0.0f;
    hy.iq_f = 0.0f;
    hy.z1_d = 0.0f;
    hy.z2_d = 0.0f;
    hy.z1_q = 0.0f;
    hy.z2_q = 0.0f;
    hy.id_int = 0.0f;
    hy.iq_int = 0.0f;
    hy.theta_prev = 0.0f;
    hy.omega_f = 0.0f;
    hy.spd_int = 0.0f;
    hy.iq_cmd_f = 0.0f;
    hy.filt_init = 0;
    hy.eso_init = 0;
    hy.omega_init = 0;
    hy._pad = 0;
    hy.magic = kHyMagic;
}

const float ts = Ts;
float ld = Ld;
float lq = Lq;
float rs = Rs;
float psi_f = PsiF;
if (!(ld > 1.0e-7f)) ld = 70.0e-6f;
if (!(lq > 1.0e-7f)) lq = 120.0e-6f;
if (!(rs > 0.0f) || !(rs < 10.0f)) rs = 0.05f;
/* Gen6 no-load ceiling @55 V matches ψ≈0.07 better than the old 0.01 placeholder. */
if (!(psi_f > 0.0f) || !(psi_f < 1.0f)) psi_f = 0.07f;
if (psi_f < 0.02f) psi_f = 0.07f; /* reject leftover 0.01 graph defaults */

float wo = OmegaO;
float db_scale = DbScale;
if (!(wo > 0.0f)) wo = 1500.0f;
if (wo > 4000.0f) wo = 4000.0f;
if (!(db_scale > 0.10f)) db_scale = 0.55f;
if (db_scale > 1.0f) db_scale = 1.0f;

const float i_base = (I_Base > 0.0f) ? I_Base : 20.0f;
float i_max = (I_Max > 0.0f) ? I_Max : 15.0f;
if (i_max < 5.0f) i_max = 5.0f;
if (i_max > 40.0f) i_max = 40.0f;

float pole_pairs = PolePairs;
if (!(pole_pairs > 0.5f) || !(pole_pairs < 40.0f)) pole_pairs = 5.0f;

float spd_kp = SpdKp;
float spd_ki = SpdKi;
if (!(spd_kp > 0.0f)) spd_kp = 0.03f;
if (!(spd_ki >= 0.0f)) spd_ki = 0.08f;
const bool spd_mode = (SpdMode > 0.5f);

const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref_in = I_Q_Ref;
const float theta_e = Theta_E;
const float pi = 3.14159265359f;
const float two_pi = 6.28318530718f;
const float sqrt3 = 1.73205080757f;

/* Local unwrapped ωe (also used by speed loop). */
float omega_e = Omega_E;
if (!(omega_e == omega_e)) omega_e = 0.0f;
if (hy.omega_init == 0) {
    hy.theta_prev = theta_e;
    hy.omega_f = 0.0f;
    hy.omega_init = 1;
} else if (ts > 0.0f) {
    float dth = theta_e - hy.theta_prev;
    dth = fmodf(dth + pi, two_pi);
    if (dth < 0.0f) dth += two_pi;
    dth -= pi;
    const float w_raw = dth / ts;
    const float fc_w = 150.0f;
    const float a_w = 1.0f / (1.0f + 1.0f / (6.28318530718f * fc_w * ts));
    hy.omega_f += a_w * (w_raw - hy.omega_f);
    omega_e = hy.omega_f;
}
hy.theta_prev = theta_e;
if (fabsf(omega_e) > 900.0f) omega_e = (omega_e > 0.0f) ? 900.0f : -900.0f;

const float rpm_meas = omega_e / pole_pairs * 60.0f / two_pi;
const float v_max = (vdc / sqrt3) * 0.95f;
/* Available headroom vs bemf — used to ease gains near the 55 V ceiling. */
float bemf = fabsf(omega_e) * psi_f;
float headroom = v_max - bemf;
if (headroom < 0.0f) headroom = 0.0f;
float db_eff = db_scale;
if (v_max > 1.0f) {
    const float util = bemf / v_max;
    if (util > 0.75f) {
        float ease = 1.0f - 0.55f * ((util - 0.75f) / 0.25f);
        if (ease < 0.45f) ease = 0.45f;
        if (ease > 1.0f) ease = 1.0f;
        db_eff *= ease;
    }
}

float iq_ref = iq_ref_in;
float id_ref_cmd = id_ref;
if (spd_mode) {
    /* I_Q_Ref interpreted as mechanical rpm setpoint. */
    float rpm_ref = iq_ref_in;
    if (rpm_ref > 700.0f) rpm_ref = 700.0f;   /* stay under ~55 V no-load ceiling */
    if (rpm_ref < -700.0f) rpm_ref = -700.0f;
    const float rpm_err = rpm_ref - rpm_meas;
    float iq_lim = 0.45f * i_max;
    if (iq_lim < 4.0f) iq_lim = 4.0f;
    if (iq_lim > 12.0f) iq_lim = 12.0f;
    /* Shrink torque authority as bemf eats the bus (prevents sync-loss blowups). */
    if (v_max > 1.0f) {
        float util = bemf / v_max;
        if (util > 0.80f) {
            float shrink = 1.0f - 0.70f * ((util - 0.80f) / 0.20f);
            if (shrink < 0.25f) shrink = 0.25f;
            iq_lim *= shrink;
        }
    }
    hy.spd_int += spd_ki * rpm_err * ts;
    if (hy.spd_int > iq_lim) hy.spd_int = iq_lim;
    if (hy.spd_int < -iq_lim) hy.spd_int = -iq_lim;
    iq_ref = spd_kp * rpm_err + hy.spd_int;
    if (iq_ref > iq_lim) iq_ref = iq_lim;
    if (iq_ref < -iq_lim) iq_ref = -iq_lim;
    /* Soft-start Iq command. */
    const float di_max = 40.0f * ts; /* 40 A/s */
    float di = iq_ref - hy.iq_cmd_f;
    if (di > di_max) di = di_max;
    if (di < -di_max) di = -di_max;
    hy.iq_cmd_f += di;
    iq_ref = hy.iq_cmd_f;
} else {
    hy.spd_int = 0.0f;
    hy.iq_cmd_f = iq_ref;
}

/* Mild field weakening when voltage-limited (Id negative). */
if (v_max > 1.0f && bemf > 0.82f * v_max) {
    float fw = (bemf / v_max - 0.82f) / 0.18f;
    if (fw > 1.0f) fw = 1.0f;
    if (fw < 0.0f) fw = 0.0f;
    const float id_fw = -fw * 0.35f * i_max;
    if (id_ref_cmd > id_fw) id_ref_cmd = id_fw;
}
id_ref = id_ref_cmd;

if (id_ref > i_max) id_ref = i_max;
if (id_ref < -i_max) id_ref = -i_max;
if (iq_ref > i_max) iq_ref = i_max;
if (iq_ref < -i_max) iq_ref = -i_max;

const float cost_track = ((id_ref - id_raw) / i_base) * ((id_ref - id_raw) / i_base)
                       + ((iq_ref - iq_raw) / i_base) * ((iq_ref - iq_raw) / i_base);

const bool enable = Enable;
const bool bus_ok = (vdc >= 40.0f);
if (!enable || !(ts > 0.0f) || !bus_ok) {
    S_A = 0.0f;
    S_B = 0.0f;
    S_C = 0.0f;
    State_Index = 0.0f;
    Pred_I_D = rte::Amperes(id_raw);
    Pred_I_Q = rte::Amperes(iq_raw);
    Cost = cost_track;
    V_Alpha = rte::Volts(0.0f);
    V_Beta = rte::Volts(0.0f);
    V_D = rte::Volts(0.0f);
    V_Q = rte::Volts(0.0f);
    Fhat_D = hy.z2_d;
    Fhat_Q = hy.z2_q;
    hy.u_alpha_prev = 0.0f;
    hy.u_beta_prev = 0.0f;
    hy.enable_s = 0.0f;
    hy.id_f = id_raw;
    hy.iq_f = iq_raw;
    hy.filt_init = 0;
    hy.eso_init = 0;
    hy.omega_init = 0;
    hy.omega_f = 0.0f;
    hy.spd_int = 0.0f;
    hy.iq_cmd_f = 0.0f;
    hy.z1_d = 0.0f;
    hy.z2_d = 0.0f;
    hy.z1_q = 0.0f;
    hy.z2_q = 0.0f;
    hy.id_int = 0.0f;
    hy.iq_int = 0.0f;
} else {
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
    const float b0_d = 1.0f / ld;
    const float b0_q = 1.0f / lq;

    if (hy.filt_init == 0) {
        hy.id_f = id_raw;
        hy.iq_f = iq_raw;
        hy.filt_init = 1;
    } else {
        const float fc = 400.0f;
        const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * fc * ts));
        hy.id_f += alpha * (id_raw - hy.id_f);
        hy.iq_f += alpha * (iq_raw - hy.iq_f);
    }
    const float id = hy.id_f;
    const float iq = hy.iq_f;

    const float vd_prev = hy.u_alpha_prev * cos_t + hy.u_beta_prev * sin_t;
    const float vq_prev = -hy.u_alpha_prev * sin_t + hy.u_beta_prev * cos_t;

    const float id_p = id + (ts / ld) * (vd_prev - rs * id + omega_e * lq * iq);
    const float iq_p = iq + (ts / lq) * (vq_prev - rs * iq - omega_e * ld * id - omega_e * psi_f);

    if (hy.eso_init == 0) {
        hy.z1_d = id;
        hy.z2_d = 0.0f;
        hy.z1_q = iq;
        hy.z2_q = 0.0f;
        hy.eso_init = 1;
    } else {
        const float vd_model = rs * id - omega_e * lq * iq;
        const float vq_model = rs * iq + omega_e * ld * id + omega_e * psi_f;
        const float vd_res = vd_prev - vd_model;
        const float vq_res = vq_prev - vq_model;
        const float beta1 = 2.0f * wo;
        const float beta2 = wo * wo;
        const float ed = hy.z1_d - id;
        const float eq = hy.z1_q - iq;
        hy.z1_d += ts * (hy.z2_d + b0_d * vd_res - beta1 * ed);
        hy.z2_d += ts * (-beta2 * ed);
        hy.z1_q += ts * (hy.z2_q + b0_q * vq_res - beta1 * eq);
        hy.z2_q += ts * (-beta2 * eq);
        const float fmax = 1.0e4f;
        if (hy.z2_d > fmax) hy.z2_d = fmax;
        if (hy.z2_d < -fmax) hy.z2_d = -fmax;
        if (hy.z2_q > fmax) hy.z2_q = fmax;
        if (hy.z2_q < -fmax) hy.z2_q = -fmax;
    }

    const float ramp_s = 0.12f;
    hy.enable_s += ts;
    if (hy.enable_s > ramp_s) hy.enable_s = ramp_s;
    float v_scale = hy.enable_s / ramp_s;

    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    if (i_meas > i_max) {
        cost += 1.0e6f;
        v_scale = 0.0f;
        hy.id_int = 0.0f;
        hy.iq_int = 0.0f;
        hy.spd_int = 0.0f;
        hy.z2_d = 0.0f;
        hy.z2_q = 0.0f;
        hy.u_alpha_prev = 0.0f;
        hy.u_beta_prev = 0.0f;
    }

    const float kp_d = (ld / ts) * db_eff;
    const float kp_q = (lq / ts) * db_eff;
    const float ki = 8.0f;

    const float id_err = id_ref - id_p;
    const float iq_err = iq_ref - iq_p;

    const float i_lim_v = v_max;
    if (v_scale > 0.0f) {
        hy.id_int += ki * id_err * ts;
        hy.iq_int += ki * iq_err * ts;
    }
    if (hy.id_int > i_lim_v) hy.id_int = i_lim_v;
    if (hy.id_int < -i_lim_v) hy.id_int = -i_lim_v;
    if (hy.iq_int > i_lim_v) hy.iq_int = i_lim_v;
    if (hy.iq_int < -i_lim_v) hy.iq_int = -i_lim_v;

    const float k_eso = 0.10f;
    float vd_ref = rs * id_p - omega_e * lq * iq_p + kp_d * id_err + hy.id_int
                 - k_eso * ld * hy.z2_d;
    float vq_ref = rs * iq_p + omega_e * ld * id_p + omega_e * psi_f + kp_q * iq_err
                 + hy.iq_int - k_eso * lq * hy.z2_q;

    vd_ref *= v_scale;
    vq_ref *= v_scale;

    /* Voltage limit with Iq priority (keep torque axis when saturated). */
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        const float vq_lim = v_max;
        if (fabsf(vq_ref) > vq_lim) {
            vq_ref = (vq_ref > 0.0f) ? vq_lim : -vq_lim;
        }
        const float vd_lim_sq = v_max * v_max - vq_ref * vq_ref;
        const float vd_lim = (vd_lim_sq > 0.0f) ? sqrtf(vd_lim_sq) : 0.0f;
        if (vd_ref > vd_lim) vd_ref = vd_lim;
        if (vd_ref < -vd_lim) vd_ref = -vd_lim;
        hy.id_int *= 0.90f;
        hy.iq_int *= 0.90f;
        if (spd_mode) hy.spd_int *= 0.98f;
    }

    const float valpha_ref = vd_ref * cos_t - vq_ref * sin_t;
    const float vbeta_ref = vd_ref * sin_t + vq_ref * cos_t;

    S_A = 0.0f;
    S_B = 0.0f;
    S_C = 0.0f;
    State_Index = spd_mode ? 1.0f : 3.0f;
    Pred_I_D = rte::Amperes(id_p);
    Pred_I_Q = rte::Amperes(iq_p);
    Cost = cost;
    V_Alpha = rte::Volts(valpha_ref);
    V_Beta = rte::Volts(vbeta_ref);
    V_D = rte::Volts(vd_ref);
    V_Q = rte::Volts(vq_ref);
    Fhat_D = hy.z2_d;
    Fhat_Q = hy.z2_q;

    hy.u_alpha_prev = valpha_ref;
    hy.u_beta_prev = vbeta_ref;
}
