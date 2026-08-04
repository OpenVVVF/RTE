/* Hybrid ADRC + predictive current control for Gen6 PMSM.
 *
 * ESO (per axis, Gao):  di/dt = f + b0 * v,  b0 = 1/L
 * Mild deadbeat voltage (same SVPWM path as FOC / MPCC Mode 3):
 *   v = ( db_scale * (i* - i)/Ts  -  f_hat ) / b0
 *
 * Hardware hardening (matched to Control.Mpcc Mode 3 lessons):
 *   - current LPF ~500 Hz
 *   - soft enable ramp
 *   - hold if Vdc < 40 V
 *   - voltage limit prefers Vq (torque)
 *   - |Id*/Iq*| clamped to I_Max
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
if (!(ld > 1.0e-7f)) ld = 70.0e-6f;
if (!(lq > 1.0e-7f)) lq = 120.0e-6f;

float wo = OmegaO;
float db_scale = DbScale;
if (!(wo > 0.0f)) wo = 2000.0f;
if (!(db_scale > 0.05f)) db_scale = 0.40f;
if (db_scale > 1.0f) db_scale = 1.0f;

const float i_base = (I_Base > 0.0f) ? I_Base : 20.0f;
const float i_max = (I_Max > 0.0f) ? I_Max : 40.0f;
const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref = I_Q_Ref;
const float theta_e = Theta_E;
(void)Omega_E;
(void)Rs;
(void)PsiF;

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
} else {
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
    const float b0_d = 1.0f / ld;
    const float b0_q = 1.0f / lq;

    /* Current LPF (~500 Hz) — Gen6 ADC noise tolerance. */
    if (hy_i_filt_init < 0.5f) {
        hy_id_f = id_raw;
        hy_iq_f = iq_raw;
        hy_i_filt_init = 1.0f;
    } else {
        const float fc = 500.0f;
        const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * fc * ts));
        hy_id_f += alpha * (id_raw - hy_id_f);
        hy_iq_f += alpha * (iq_raw - hy_iq_f);
    }
    const float id = hy_id_f;
    const float iq = hy_iq_f;

    /* Previous applied dq voltage for ESO input. */
    const float vd_prev = hy_u_alpha_prev * cos_t + hy_u_beta_prev * sin_t;
    const float vq_prev = -hy_u_alpha_prev * sin_t + hy_u_beta_prev * cos_t;

    /* ESO update (ADRC). */
    if (hy_eso_init < 0.5f) {
        hy_z1_d = id;
        hy_z2_d = 0.0f;
        hy_z1_q = iq;
        hy_z2_q = 0.0f;
        hy_eso_init = 1.0f;
    } else {
        const float beta1 = 2.0f * wo;
        const float beta2 = wo * wo;
        const float ed = hy_z1_d - id;
        const float eq = hy_z1_q - iq;
        hy_z1_d += ts * (hy_z2_d + b0_d * vd_prev - beta1 * ed);
        hy_z2_d += ts * (-beta2 * ed);
        hy_z1_q += ts * (hy_z2_q + b0_q * vq_prev - beta1 * eq);
        hy_z2_q += ts * (-beta2 * eq);
    }

    /* Soft enable ramp (0.25 s). */
    const float ramp_s = 0.25f;
    hy_enable_s += ts;
    if (hy_enable_s > ramp_s) hy_enable_s = ramp_s;
    const float ramp = hy_enable_s / ramp_s;

    float fb_scale = ramp;
    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    if (i_meas > i_max) {
        cost += 1.0e6f;
        if (i_meas > 1.0e-3f) fb_scale *= i_max / i_meas;
    }

    /* Hybrid law: cancel f_hat, mild deadbeat tracking. */
    float vd_ref = ((db_scale * (id_ref - id) / ts) - hy_z2_d) / b0_d;
    float vq_ref = ((db_scale * (iq_ref - iq) / ts) - hy_z2_q) / b0_q;
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

    const float pred_id = id + ts * (hy_z2_d + b0_d * vd_ref);
    const float pred_iq = iq + ts * (hy_z2_q + b0_q * vq_ref);

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
