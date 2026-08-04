/* Hybrid ADRC + predictive current control for Gen6 PMSM.
 *
 * Spin-oriented law (matches working MPCC Mode 3 voltage path):
 *   v_dq = R i + w cross/PM terms + db*(L/Ts)(i* - i) - k_eso*L*f_hat
 * ESO estimates residual disturbance only; f_hat is clamped and lightly applied.
 *
 * Hardware hardening:
 *   - current LPF ~400 Hz
 *   - soft enable ramp
 *   - hold if Vdc < 40 V
 *   - hard current foldback for clean phase current
 *   - voltage limit prefers Vq (torque)
 *   - abs(Id_ref) and abs(Iq_ref) clamped to I_Max
 */

static float hy_u_alpha_prev = 0.0f;
static float hy_u_beta_prev = 0.0f;
static float hy_enable_s = 0.0f;
static float hy_id_f = 0.0f;
static float hy_iq_f = 0.0f;
static float hy_i_filt_init = 0.0f;
static float hy_z1_d = 0.0f;
static float hy_z2_d = 0.0f;
static float hy_z1_q = 0.0f;
static float hy_z2_q = 0.0f;
static float hy_eso_init = 0.0f;

const float ts = Ts;
float ld = Ld;
float lq = Lq;
float rs = Rs;
float psi_f = PsiF;
if (!(ld > 1.0e-7f)) ld = 70.0e-6f;
if (!(lq > 1.0e-7f)) lq = 120.0e-6f;
if (!(rs > 0.0f) || !(rs < 10.0f)) rs = 0.05f;
if (!(psi_f > 0.0f) || !(psi_f < 1.0f)) psi_f = 0.01f;

float wo = OmegaO;
float db_scale = DbScale;
if (!(wo > 0.0f)) wo = 1200.0f;
if (wo > 4000.0f) wo = 4000.0f;
if (!(db_scale > 0.05f)) db_scale = 0.20f;
if (db_scale > 0.50f) db_scale = 0.50f;

const float i_base = (I_Base > 0.0f) ? I_Base : 20.0f;
float i_max = (I_Max > 0.0f) ? I_Max : 15.0f;
if (i_max > 25.0f) i_max = 25.0f;

const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref = I_Q_Ref;
const float theta_e = Theta_E;
float omega_e = Omega_E;
if (!(omega_e == omega_e)) omega_e = 0.0f; /* NaN guard */
if (fabsf(omega_e) > 2000.0f) omega_e = (omega_e > 0.0f) ? 2000.0f : -2000.0f;

const bool enable = Enable;
const float sqrt3 = 1.73205080757f;
const float inv_sqrt3 = 0.57735026919f;
const float two_thirds = 2.0f / 3.0f;
const int switch_bits[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}
};

if (id_ref > i_max) id_ref = i_max;
if (id_ref < -i_max) id_ref = -i_max;
if (iq_ref > i_max) iq_ref = i_max;
if (iq_ref < -i_max) iq_ref = -i_max;

const float cost_track = ((id_ref - id_raw) / i_base) * ((id_ref - id_raw) / i_base)
                       + ((iq_ref - iq_raw) / i_base) * ((iq_ref - iq_raw) / i_base);

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
    Fhat_D = hy_z2_d;
    Fhat_Q = hy_z2_q;
    hy_u_alpha_prev = 0.0f;
    hy_u_beta_prev = 0.0f;
    hy_enable_s = 0.0f;
    hy_id_f = id_raw;
    hy_iq_f = iq_raw;
    hy_i_filt_init = 0.0f;
    hy_eso_init = 0.0f;
    hy_z2_d = 0.0f;
    hy_z2_q = 0.0f;
} else {
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
    const float b0_d = 1.0f / ld;
    const float b0_q = 1.0f / lq;

    /* Current LPF (~400 Hz) for cleaner phase/dq feedback. */
    if (hy_i_filt_init < 0.5f) {
        hy_id_f = id_raw;
        hy_iq_f = iq_raw;
        hy_i_filt_init = 1.0f;
    } else {
        const float fc = 400.0f;
        const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * fc * ts));
        hy_id_f += alpha * (id_raw - hy_id_f);
        hy_iq_f += alpha * (iq_raw - hy_iq_f);
    }
    const float id = hy_id_f;
    const float iq = hy_iq_f;

    /* Previous applied dq voltage for ESO input. */
    const float vd_prev = hy_u_alpha_prev * cos_t + hy_u_beta_prev * sin_t;
    const float vq_prev = -hy_u_alpha_prev * sin_t + hy_u_beta_prev * cos_t;

    /* ESO update on residual plant (after removing known model terms). */
    if (hy_eso_init < 0.5f) {
        hy_z1_d = id;
        hy_z2_d = 0.0f;
        hy_z1_q = iq;
        hy_z2_q = 0.0f;
        hy_eso_init = 1.0f;
    } else {
        const float vd_model = rs * id - omega_e * lq * iq;
        const float vq_model = rs * iq + omega_e * ld * id + omega_e * psi_f;
        const float vd_res = vd_prev - vd_model;
        const float vq_res = vq_prev - vq_model;
        const float beta1 = 2.0f * wo;
        const float beta2 = wo * wo;
        const float ed = hy_z1_d - id;
        const float eq = hy_z1_q - iq;
        hy_z1_d += ts * (hy_z2_d + b0_d * vd_res - beta1 * ed);
        hy_z2_d += ts * (-beta2 * ed);
        hy_z1_q += ts * (hy_z2_q + b0_q * vq_res - beta1 * eq);
        hy_z2_q += ts * (-beta2 * eq);
        /* Clamp disturbance (di/dt). Prevents voltage blow-up. */
        const float fmax = 5.0e4f;
        if (hy_z2_d > fmax) hy_z2_d = fmax;
        if (hy_z2_d < -fmax) hy_z2_d = -fmax;
        if (hy_z2_q > fmax) hy_z2_q = fmax;
        if (hy_z2_q < -fmax) hy_z2_q = -fmax;
    }

    /* Soft enable ramp (0.5 s) for clean start. */
    const float ramp_s = 0.50f;
    hy_enable_s += ts;
    if (hy_enable_s > ramp_s) hy_enable_s = ramp_s;
    float ramp = hy_enable_s / ramp_s;

    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    float fb_scale = ramp;

    /* Progressive current foldback — keeps phase current clean. */
    const float i_warn = 0.70f * i_max;
    if (i_meas > i_warn) {
        cost += 1.0e5f;
        float denom = i_max - i_warn;
        if (!(denom > 1.0e-3f)) denom = 1.0e-3f;
        const float over = (i_meas - i_warn) / denom;
        float shrink = 1.0f - 0.85f * over;
        if (shrink < 0.05f) shrink = 0.05f;
        fb_scale *= shrink;
    }
    if (i_meas > i_max) {
        cost += 1.0e6f;
        fb_scale *= 0.05f;
    }

    /* Mode-3 style model + mild deadbeat + light ESO residual cancel. */
    const float k_eso = 0.20f;
    float vd_ref = rs * id - omega_e * lq * iq
                 + db_scale * (ld / ts) * (id_ref - id)
                 - k_eso * ld * hy_z2_d;
    float vq_ref = rs * iq + omega_e * ld * id + omega_e * psi_f
                 + db_scale * (lq / ts) * (iq_ref - iq)
                 - k_eso * lq * hy_z2_q;
    vd_ref *= fb_scale;
    vq_ref *= fb_scale;

    const float v_max = (vdc / sqrt3) * 0.95f;
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        if (fabsf(vq_ref) >= v_max) {
            vq_ref = (vq_ref >= 0.0f) ? v_max : -v_max;
            vd_ref = 0.0f;
        } else {
            const float vd_lim = sqrtf(v_max * v_max - vq_ref * vq_ref);
            if (vd_ref > vd_lim) vd_ref = vd_lim;
            if (vd_ref < -vd_lim) vd_ref = -vd_lim;
        }
    }

    const float valpha_ref = vd_ref * cos_t - vq_ref * sin_t;
    const float vbeta_ref = vd_ref * sin_t + vq_ref * cos_t;

    int best_idx = 1;
    float best_dist = 1.0e30f;
    for (int idx = 1; idx <= 6; ++idx) {
        const float sa = static_cast<float>(switch_bits[idx][0]);
        const float sb = static_cast<float>(switch_bits[idx][1]);
        const float sc = static_cast<float>(switch_bits[idx][2]);
        const float ua = two_thirds * vdc * (sa - 0.5f * (sb + sc));
        const float ub = vdc * inv_sqrt3 * (sb - sc);
        const float dist = (ua - valpha_ref) * (ua - valpha_ref)
                         + (ub - vbeta_ref) * (ub - vbeta_ref);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = idx;
        }
    }

    const float pred_id = id + ts * ((vd_ref - rs * id + omega_e * lq * iq) / ld);
    const float pred_iq = iq + ts * ((vq_ref - rs * iq - omega_e * ld * id - omega_e * psi_f) / lq);

    S_A = static_cast<float>(switch_bits[best_idx][0]);
    S_B = static_cast<float>(switch_bits[best_idx][1]);
    S_C = static_cast<float>(switch_bits[best_idx][2]);
    State_Index = static_cast<float>(best_idx);
    Pred_I_D = rte::Amperes(pred_id);
    Pred_I_Q = rte::Amperes(pred_iq);
    Cost = cost;
    V_Alpha = rte::Volts(valpha_ref);
    V_Beta = rte::Volts(vbeta_ref);
    V_D = rte::Volts(vd_ref);
    V_Q = rte::Volts(vq_ref);
    Fhat_D = hy_z2_d;
    Fhat_Q = hy_z2_q;

    hy_u_alpha_prev = valpha_ref;
    hy_u_beta_prev = vbeta_ref;
}
