/* Hybrid ADRC + predictive current control for Gen6 PMSM.
 *
 * Based on the Mode-3 law that actually spins Gen6 (deadbeat + Ki), with a
 * mild residual ESO (ADRC) correction.
 *
 * State is in AXISRAM (.dma_buffers) to avoid DTCM overflow. That section is
 * NOLOAD (not zeroed at boot) — validate magic and clear before use, or
 * garbage integrators command huge voltage (HybridADRCMPC-3 spikes).
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
    uint8_t filt_init;
    uint8_t eso_init;
    uint8_t _pad[2];
};

static constexpr uint32_t kHyMagic = 0x48594133u; /* HYA3 */

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
    hy.filt_init = 0;
    hy.eso_init = 0;
    hy._pad[0] = 0;
    hy._pad[1] = 0;
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
if (!(psi_f > 0.0f) || !(psi_f < 1.0f)) psi_f = 0.01f;

float wo = OmegaO;
float db_scale = DbScale;
if (!(wo > 0.0f)) wo = 1500.0f;
if (wo > 4000.0f) wo = 4000.0f;
if (!(db_scale > 0.10f)) db_scale = 0.70f;
if (db_scale > 1.0f) db_scale = 1.0f;

const float i_base = (I_Base > 0.0f) ? I_Base : 20.0f;
float i_max = (I_Max > 0.0f) ? I_Max : 25.0f;
if (i_max < 5.0f) i_max = 5.0f;
if (i_max > 40.0f) i_max = 40.0f;

const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref = I_Q_Ref;
const float theta_e = Theta_E;
float omega_e = Omega_E;
if (!(omega_e == omega_e)) omega_e = 0.0f;
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
    Fhat_D = hy.z2_d;
    Fhat_Q = hy.z2_q;
    hy.u_alpha_prev = 0.0f;
    hy.u_beta_prev = 0.0f;
    hy.enable_s = 0.0f;
    hy.id_f = id_raw;
    hy.iq_f = iq_raw;
    hy.filt_init = 0;
    hy.eso_init = 0;
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
        const float fc = 500.0f;
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
        const float fmax = 2.0e4f;
        if (hy.z2_d > fmax) hy.z2_d = fmax;
        if (hy.z2_d < -fmax) hy.z2_d = -fmax;
        if (hy.z2_q > fmax) hy.z2_q = fmax;
        if (hy.z2_q < -fmax) hy.z2_q = -fmax;
    }

    const float ramp_s = 0.08f;
    hy.enable_s += ts;
    if (hy.enable_s > ramp_s) hy.enable_s = ramp_s;
    float v_scale = hy.enable_s / ramp_s;

    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    if (i_meas > i_max) {
        cost += 1.0e6f;
        if (i_meas > 1.0e-3f) {
            v_scale *= i_max / i_meas;
        }
        if (v_scale < 0.15f) v_scale = 0.15f;
    }

    const float kp_d = (ld / ts) * db_scale;
    const float kp_q = (lq / ts) * db_scale;
    const float ki = 10.0f;

    const float id_err = id_ref - id_p;
    const float iq_err = iq_ref - iq_p;

    const float i_lim_v = (vdc / sqrt3) * 0.95f;
    hy.id_int += ki * id_err * ts;
    hy.iq_int += ki * iq_err * ts;
    if (hy.id_int > i_lim_v) hy.id_int = i_lim_v;
    if (hy.id_int < -i_lim_v) hy.id_int = -i_lim_v;
    if (hy.iq_int > i_lim_v) hy.iq_int = i_lim_v;
    if (hy.iq_int < -i_lim_v) hy.iq_int = -i_lim_v;

    const float k_eso = 0.15f;
    float vd_ref = rs * id_p - omega_e * lq * iq_p + kp_d * id_err + hy.id_int
                 - k_eso * ld * hy.z2_d;
    float vq_ref = rs * iq_p + omega_e * ld * id_p + omega_e * psi_f + kp_q * iq_err
                 + hy.iq_int - k_eso * lq * hy.z2_q;

    vd_ref *= v_scale;
    vq_ref *= v_scale;

    const float v_max = (vdc / sqrt3) * 0.95f;
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        const float scale = v_max / sqrtf(v_mag_sq);
        vd_ref *= scale;
        vq_ref *= scale;
        hy.id_int *= 0.95f;
        hy.iq_int *= 0.95f;
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

    S_A = static_cast<float>(switch_bits[best_idx][0]);
    S_B = static_cast<float>(switch_bits[best_idx][1]);
    S_C = static_cast<float>(switch_bits[best_idx][2]);
    State_Index = static_cast<float>(best_idx);
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
