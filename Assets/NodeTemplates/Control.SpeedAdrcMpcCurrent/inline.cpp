/* Cascaded Gen6 controller (NEW node — does not modify Control.AdrcMpcc):
 *
 *   Outer: LADRC on mechanical speed (rpm) → Iq*
 *   Inner: Mode-3 deadbeat / predictive current → Vαβ → SVPWM
 *
 * CascadeMode:
 *   0 = current-only (FOC-like): I_Q_Ref in amperes, speed ADRC bypassed
 *   1 = cascaded: Spd_Ref in rpm → speed ADRC → Iq*, then MPC current
 *
 * State in AXISRAM (.dma_buffers, NOLOAD) — magic-clear before use.
 * Do NOT add extra Control.Slew nodes for rpm (each Slew burns ~16 B DTCM).
 */

struct SpdAdrcMpcState {
    uint32_t magic;
    float u_alpha_prev;
    float u_beta_prev;
    float enable_s;
    float id_f;
    float iq_f;
    float theta_prev;
    float omega_f;
    float id_int;
    float iq_int;
    /* Speed LADRC states (y = rpm, u = Iq). */
    float z1;
    float z2;
    float u_iq_prev;
    float spd_cmd; /* internal rpm slew (keeps DTCM free of SlewSpd) */
    uint8_t filt_init;
    uint8_t omega_init;
    uint8_t spd_init;
    uint8_t _pad;
};

static constexpr uint32_t kSamMagic = 0x53414D32u; /* SAM2 */

static SpdAdrcMpcState st __attribute__((section(".dma_buffers"), aligned(4)));

if (st.magic != kSamMagic) {
    st.u_alpha_prev = 0.0f;
    st.u_beta_prev = 0.0f;
    st.enable_s = 0.0f;
    st.id_f = 0.0f;
    st.iq_f = 0.0f;
    st.theta_prev = 0.0f;
    st.omega_f = 0.0f;
    st.id_int = 0.0f;
    st.iq_int = 0.0f;
    st.z1 = 0.0f;
    st.z2 = 0.0f;
    st.u_iq_prev = 0.0f;
    st.spd_cmd = 0.0f;
    st.filt_init = 0;
    st.omega_init = 0;
    st.spd_init = 0;
    st._pad = 0;
    st.magic = kSamMagic;
}

const float ts = Ts;
float ld = Ld;
float lq = Lq;
float rs = Rs;
float psi_f = PsiF;
if (!(ld > 1.0e-7f)) ld = 70.0e-6f;
if (!(lq > 1.0e-7f)) lq = 120.0e-6f;
if (!(rs > 0.0f) || !(rs < 10.0f)) rs = 0.05f;
if (!(psi_f > 0.0f) || !(psi_f < 1.0f)) psi_f = 0.07f;
if (psi_f < 0.02f) psi_f = 0.07f;

float db_scale = DbScale;
if (!(db_scale > 0.10f)) db_scale = 0.55f;
if (db_scale > 1.0f) db_scale = 1.0f;

float i_max = (I_Max > 0.0f) ? I_Max : 15.0f;
if (i_max < 5.0f) i_max = 5.0f;
if (i_max > 40.0f) i_max = 40.0f;

float pole_pairs = PolePairs;
if (!(pole_pairs > 0.5f) || !(pole_pairs < 40.0f)) pole_pairs = 5.0f;

float wc = SpdOmegaC;
float wo = SpdOmegaO;
float b0_spd = SpdB0;
if (!(wc > 0.0f)) wc = 40.0f;
if (!(wo > 0.0f)) wo = 120.0f;
if (!(b0_spd > 1.0e-4f)) b0_spd = 80.0f; /* rpm/s per amp — tune on hardware */

const bool cascade = (Cascade_Mode > 0.5f);
const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref_in = I_Q_Ref;
float spd_ref = Spd_Ref;
const float theta_e = Theta_E;
const float pi = 3.14159265359f;
const float two_pi = 6.28318530718f;
const float sqrt3 = 1.73205080757f;

/* Unwrapped electrical speed. */
float omega_e = Omega_E;
if (!(omega_e == omega_e)) omega_e = 0.0f;
if (st.omega_init == 0) {
    st.theta_prev = theta_e;
    st.omega_f = 0.0f;
    st.omega_init = 1;
} else if (ts > 0.0f) {
    float dth = theta_e - st.theta_prev;
    dth = fmodf(dth + pi, two_pi);
    if (dth < 0.0f) dth += two_pi;
    dth -= pi;
    const float w_raw = dth / ts;
    const float a_w = 1.0f / (1.0f + 1.0f / (6.28318530718f * 150.0f * ts));
    st.omega_f += a_w * (w_raw - st.omega_f);
    omega_e = st.omega_f;
}
st.theta_prev = theta_e;
if (fabsf(omega_e) > 900.0f) omega_e = (omega_e > 0.0f) ? 900.0f : -900.0f;

const float rpm_meas = omega_e / pole_pairs * 60.0f / two_pi;
const float v_max = (vdc / sqrt3) * 0.95f;

/* ---- Outer speed LADRC (cascade only) → Iq* ---- */
float iq_ref = iq_ref_in;
if (cascade) {
    if (spd_ref > 700.0f) spd_ref = 700.0f;
    if (spd_ref < -700.0f) spd_ref = -700.0f;

    float iq_lim = 0.50f * i_max;
    if (iq_lim < 4.0f) iq_lim = 4.0f;
    if (iq_lim > 12.0f) iq_lim = 12.0f;
    /* Shrink torque near voltage ceiling. */
    const float bemf = fabsf(omega_e) * psi_f;
    if (v_max > 1.0f && bemf > 0.80f * v_max) {
        float shrink = 1.0f - 0.70f * ((bemf / v_max - 0.80f) / 0.20f);
        if (shrink < 0.25f) shrink = 0.25f;
        iq_lim *= shrink;
    }

    if (st.spd_init == 0) {
        st.z1 = rpm_meas;
        st.z2 = 0.0f;
        st.u_iq_prev = 0.0f;
        st.spd_cmd = rpm_meas;
        st.spd_init = 1;
    }

    /* Slew rpm setpoint in AXISRAM (~120 rpm/s) — avoids a DTCM Slew node. */
    {
        const float dmax = 120.0f * ts;
        float d = spd_ref - st.spd_cmd;
        if (d > dmax) d = dmax;
        if (d < -dmax) d = -dmax;
        st.spd_cmd += d;
        spd_ref = st.spd_cmd;
    }

    const float beta1 = 2.0f * wo;
    const float beta2 = wo * wo;
    const float e_obs = st.z1 - rpm_meas;
    st.z1 += ts * (st.z2 + b0_spd * st.u_iq_prev - beta1 * e_obs);
    st.z2 += ts * (-beta2 * e_obs);
    const float fmax = 5.0e4f;
    if (st.z2 > fmax) st.z2 = fmax;
    if (st.z2 < -fmax) st.z2 = -fmax;

    const float u0 = wc * (spd_ref - st.z1);
    float u_iq = (u0 - st.z2) / b0_spd;
    if (u_iq > iq_lim) u_iq = iq_lim;
    if (u_iq < -iq_lim) u_iq = -iq_lim;
    st.u_iq_prev = u_iq;
    iq_ref = u_iq;
} else {
    st.spd_init = 0;
    st.z1 = 0.0f;
    st.z2 = 0.0f;
    st.u_iq_prev = 0.0f;
    st.spd_cmd = 0.0f;
}

if (id_ref > i_max) id_ref = i_max;
if (id_ref < -i_max) id_ref = -i_max;
if (iq_ref > i_max) iq_ref = i_max;
if (iq_ref < -i_max) iq_ref = -i_max;

const float i_base = (I_Base > 0.0f) ? I_Base : 20.0f;
const float cost_track = ((id_ref - id_raw) / i_base) * ((id_ref - id_raw) / i_base)
                       + ((iq_ref - iq_raw) / i_base) * ((iq_ref - iq_raw) / i_base);

const bool enable = Enable;
const bool bus_ok = (vdc >= 40.0f);

Iq_Cmd = iq_ref;
Rpm_Meas = rpm_meas;

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
    st.u_alpha_prev = 0.0f;
    st.u_beta_prev = 0.0f;
    st.enable_s = 0.0f;
    st.id_f = id_raw;
    st.iq_f = iq_raw;
    st.filt_init = 0;
    st.omega_init = 0;
    st.spd_init = 0;
    st.omega_f = 0.0f;
    st.id_int = 0.0f;
    st.iq_int = 0.0f;
    st.z1 = 0.0f;
    st.z2 = 0.0f;
    st.u_iq_prev = 0.0f;
    st.spd_cmd = 0.0f;
} else {
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);

    if (st.filt_init == 0) {
        st.id_f = id_raw;
        st.iq_f = iq_raw;
        st.filt_init = 1;
    } else {
        const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * 400.0f * ts));
        st.id_f += alpha * (id_raw - st.id_f);
        st.iq_f += alpha * (iq_raw - st.iq_f);
    }
    const float id = st.id_f;
    const float iq = st.iq_f;

    const float vd_prev = st.u_alpha_prev * cos_t + st.u_beta_prev * sin_t;
    const float vq_prev = -st.u_alpha_prev * sin_t + st.u_beta_prev * cos_t;

    /* One-step delay-compensated prediction (MPC / Mode-3). */
    const float id_p = id + (ts / ld) * (vd_prev - rs * id + omega_e * lq * iq);
    const float iq_p = iq + (ts / lq) * (vq_prev - rs * iq - omega_e * ld * id - omega_e * psi_f);

    const float ramp_s = 0.12f;
    st.enable_s += ts;
    if (st.enable_s > ramp_s) st.enable_s = ramp_s;
    float v_scale = st.enable_s / ramp_s;

    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    if (i_meas > i_max) {
        cost += 1.0e6f;
        v_scale = 0.0f;
        st.id_int = 0.0f;
        st.iq_int = 0.0f;
        st.u_iq_prev = 0.0f;
        st.u_alpha_prev = 0.0f;
        st.u_beta_prev = 0.0f;
    }

    float db_eff = db_scale;
    const float bemf = fabsf(omega_e) * psi_f;
    if (v_max > 1.0f && bemf > 0.75f * v_max) {
        float ease = 1.0f - 0.55f * ((bemf / v_max - 0.75f) / 0.25f);
        if (ease < 0.45f) ease = 0.45f;
        db_eff *= ease;
    }

    const float kp_d = (ld / ts) * db_eff;
    const float kp_q = (lq / ts) * db_eff;
    const float ki = 8.0f;
    const float id_err = id_ref - id_p;
    const float iq_err = iq_ref - iq_p;

    if (v_scale > 0.0f) {
        st.id_int += ki * id_err * ts;
        st.iq_int += ki * iq_err * ts;
    }
    if (st.id_int > v_max) st.id_int = v_max;
    if (st.id_int < -v_max) st.id_int = -v_max;
    if (st.iq_int > v_max) st.iq_int = v_max;
    if (st.iq_int < -v_max) st.iq_int = -v_max;

    float vd_ref = rs * id_p - omega_e * lq * iq_p + kp_d * id_err + st.id_int;
    float vq_ref = rs * iq_p + omega_e * ld * id_p + omega_e * psi_f + kp_q * iq_err + st.iq_int;

    vd_ref *= v_scale;
    vq_ref *= v_scale;

    /* Iq-priority voltage limit. */
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        if (fabsf(vq_ref) > v_max) {
            vq_ref = (vq_ref > 0.0f) ? v_max : -v_max;
        }
        const float vd_lim_sq = v_max * v_max - vq_ref * vq_ref;
        const float vd_lim = (vd_lim_sq > 0.0f) ? sqrtf(vd_lim_sq) : 0.0f;
        if (vd_ref > vd_lim) vd_ref = vd_lim;
        if (vd_ref < -vd_lim) vd_ref = -vd_lim;
        st.id_int *= 0.90f;
        st.iq_int *= 0.90f;
    }

    const float valpha_ref = vd_ref * cos_t - vq_ref * sin_t;
    const float vbeta_ref = vd_ref * sin_t + vq_ref * cos_t;

    S_A = 0.0f;
    S_B = 0.0f;
    S_C = 0.0f;
    State_Index = cascade ? 1.0f : 0.0f;
    Pred_I_D = rte::Amperes(id_p);
    Pred_I_Q = rte::Amperes(iq_p);
    Cost = cost;
    V_Alpha = rte::Volts(valpha_ref);
    V_Beta = rte::Volts(vbeta_ref);
    V_D = rte::Volts(vd_ref);
    V_Q = rte::Volts(vq_ref);

    st.u_alpha_prev = valpha_ref;
    st.u_beta_prev = vbeta_ref;
}
