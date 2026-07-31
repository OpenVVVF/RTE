/* Predictive current control for three-phase PMSM (Zhang et al., IEEE TIA 2017).
 * Mode: 0=Conventional FCS (full vector / period)
 *       1=Delay-compensated FCS
 *       2=Back-EMF path (same predictor hook as 1 for now)
 *       3=Deadbeat-style voltage → SVPWM (use on Gen6 / low-L motors)
 *
 * Mode 3 notes (Gen6 hardware):
 * A pure one-step deadbeat with Ts_Eff = 0.1*Ts and a large residual Ki
 * is ~100–300× more aggressive than the working FOC PI (Kp≈0.03, Ki≈10).
 * That produces duty-cycle chatter and shaft vibration. We therefore:
 *   (1) use Kp = (L/Ts)*KpScale with KpScale < 1 (stability margin),
 *   (2) use FOC-like Ki,
 *   (3) low-pass the voltage command,
 *   (4) soft current foldback instead of a hard V=0 trip (bang-bang).
 */

static float mpcc_prev_sa = 0.0f;
static float mpcc_prev_sb = 0.0f;
static float mpcc_prev_sc = 0.0f;
static float mpcc_u_alpha_prev = 0.0f;
static float mpcc_u_beta_prev = 0.0f;
static float mpcc_id_int = 0.0f;
static float mpcc_iq_int = 0.0f;
static float mpcc_vd_filt = 0.0f;
static float mpcc_vq_filt = 0.0f;

const float ts = Ts;
const float rs = Rs;
const float ld = Ld;
const float lq = Lq;
const float psi_f = PsiF;
const float i_base = (I_Base > 0.0f) ? I_Base : 10.0f;
const float i_max = (I_Max > 0.0f) ? I_Max : 30.0f;
const float vdc = V_Dc.in(au::volts);
const float id = I_D.in(au::amperes);
const float iq = I_Q.in(au::amperes);
const float id_ref = I_D_Ref;
const float iq_ref = I_Q_Ref;
const float theta_e = Theta_E;
const float omega_e = Omega_E;
const bool enable = Enable > 0.5f;
const int mode_i = (Mode >= 3.0f) ? 3 : ((Mode >= 2.0f) ? 2 : ((Mode >= 1.0f) ? 1 : 0));

const float two_thirds = 2.0f / 3.0f;
const float inv_sqrt3 = 0.57735026919f;
const int switch_bits[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}
};

/* Tracking cost always reflects the measured error (even when disabled). */
const float cost_track = ((id_ref - id) / i_base) * ((id_ref - id) / i_base)
                       + ((iq_ref - iq) / i_base) * ((iq_ref - iq) / i_base);

if (!enable || !(ts > 0.0f) || !(vdc > 0.0f) || ld <= 0.0f || lq <= 0.0f) {
    S_A = 0.0f;
    S_B = 0.0f;
    S_C = 0.0f;
    State_Index = 0.0f;
    Pred_I_D = rte::Amperes(id);
    Pred_I_Q = rte::Amperes(iq);
    Cost = cost_track;
    V_Alpha = rte::Volts(0.0f);
    V_Beta = rte::Volts(0.0f);
    V_D = rte::Volts(0.0f);
    V_Q = rte::Volts(0.0f);
    mpcc_prev_sa = 0.0f;
    mpcc_prev_sb = 0.0f;
    mpcc_prev_sc = 0.0f;
    mpcc_u_alpha_prev = 0.0f;
    mpcc_u_beta_prev = 0.0f;
    mpcc_id_int = 0.0f;
    mpcc_iq_int = 0.0f;
    mpcc_vd_filt = 0.0f;
    mpcc_vq_filt = 0.0f;
} else if (mode_i == 3) {
    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    if (i_meas > i_max) cost += 1.0e6f;

    /* Proportional gain: fraction of deadbeat L/Ts. FOC uses ~0.03 V/A;
     * full deadbeat on Gen6 L is ~1 V/A — still hot, so keep a margin. */
    const float kp_scale = 0.25f;
    const float kp_d = (ld / ts) * kp_scale;
    const float kp_q = (lq / ts) * kp_scale;
    const float ki = 10.0f; /* matches foc_demo default Ki */

    const float id_err = id_ref - id;
    const float iq_err = iq_ref - iq;

    /* Integrate only while comfortably under the current limit. */
    const float i_lim_v = 0.25f * vdc;
    if (i_meas < i_max * 0.85f) {
        mpcc_id_int += ki * id_err * ts;
        mpcc_iq_int += ki * iq_err * ts;
    } else {
        mpcc_id_int *= 0.95f;
        mpcc_iq_int *= 0.95f;
    }
    if (mpcc_id_int > i_lim_v) mpcc_id_int = i_lim_v;
    if (mpcc_id_int < -i_lim_v) mpcc_id_int = -i_lim_v;
    if (mpcc_iq_int > i_lim_v) mpcc_iq_int = i_lim_v;
    if (mpcc_iq_int < -i_lim_v) mpcc_iq_int = -i_lim_v;

    float vd_ref = rs * id - omega_e * lq * iq + kp_d * id_err + mpcc_id_int;
    float vq_ref = rs * iq + omega_e * ld * id + omega_e * psi_f + kp_q * iq_err + mpcc_iq_int;

    /* Soft foldback near I_Max — scales voltage down instead of V=0 trip,
     * which previously bang-banged and vibrated the shaft. */
    if (i_meas > i_max * 0.85f && i_meas > 1.0e-3f) {
        const float scale = (i_max * 0.85f) / i_meas;
        vd_ref *= scale;
        vq_ref *= scale;
    }

    const float v_max = (vdc * inv_sqrt3) * 0.95f;
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        const float scale = v_max / sqrtf(v_mag_sq);
        vd_ref *= scale;
        vq_ref *= scale;
        mpcc_id_int *= 0.95f;
        mpcc_iq_int *= 0.95f;
    }

    /* First-order LPF on voltage (~α=0.2 @ 5 kHz → ~1.6 kHz bandwidth). */
    const float alpha_v = 0.2f;
    mpcc_vd_filt += alpha_v * (vd_ref - mpcc_vd_filt);
    mpcc_vq_filt += alpha_v * (vq_ref - mpcc_vq_filt);
    vd_ref = mpcc_vd_filt;
    vq_ref = mpcc_vq_filt;

    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
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

    const float pred_id = id + (ts / ld) * (vd_ref - rs * id + omega_e * lq * iq);
    const float pred_iq = iq + (ts / lq) * (vq_ref - rs * iq - omega_e * ld * id - omega_e * psi_f);

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

    mpcc_prev_sa = S_A;
    mpcc_prev_sb = S_B;
    mpcc_prev_sc = S_C;
    mpcc_u_alpha_prev = valpha_ref;
    mpcc_u_beta_prev = vbeta_ref;
} else {
    float id_base = id;
    float iq_base = iq;
    if (mode_i >= 1) {
        const float di_d_sp = (mpcc_u_alpha_prev - rs * id) / lq;
        const float di_q_sp = (mpcc_u_beta_prev - rs * iq) / lq;
        const float id_sp = id + ts * di_d_sp;
        const float iq_sp = iq + ts * di_q_sp;
        id_base = id_sp + (-rs * (id_sp - id) * ts) / (2.0f * lq);
        iq_base = iq_sp + (-rs * (iq_sp - iq) * ts) / (2.0f * lq);
    }

    float best_cost = 1.0e30f;
    int best_idx = 0;
    float best_pred_id = id;
    float best_pred_iq = iq;
    float best_valpha = 0.0f;
    float best_vbeta = 0.0f;

    for (int idx = 0; idx < 8; ++idx) {
        const float sa = static_cast<float>(switch_bits[idx][0]);
        const float sb = static_cast<float>(switch_bits[idx][1]);
        const float sc = static_cast<float>(switch_bits[idx][2]);
        const float valpha = two_thirds * vdc * (sa - 0.5f * (sb + sc));
        const float vbeta = vdc * inv_sqrt3 * (sb - sc);

        const float cos_t = cosf(theta_e);
        const float sin_t = sinf(theta_e);
        const float vd = valpha * cos_t + vbeta * sin_t;
        const float vq = -valpha * sin_t + vbeta * cos_t;

        const float pred_id = id_base + (ts / ld) * (vd - rs * id_base + omega_e * lq * iq_base);
        const float pred_iq = iq_base + (ts / lq) * (vq - rs * iq_base - omega_e * ld * id_base - omega_e * psi_f);

        float cost = ((id_ref - pred_id) / i_base) * ((id_ref - pred_id) / i_base)
                   + ((iq_ref - pred_iq) / i_base) * ((iq_ref - pred_iq) / i_base);
        const float imag = sqrtf(pred_id * pred_id + pred_iq * pred_iq);
        if (imag > i_max) cost += 1.0e6f;

        const int trans = ((sa != mpcc_prev_sa) ? 1 : 0)
                        + ((sb != mpcc_prev_sb) ? 1 : 0)
                        + ((sc != mpcc_prev_sc) ? 1 : 0);
        const int best_trans =
            ((switch_bits[best_idx][0] != static_cast<int>(mpcc_prev_sa)) ? 1 : 0)
          + ((switch_bits[best_idx][1] != static_cast<int>(mpcc_prev_sb)) ? 1 : 0)
          + ((switch_bits[best_idx][2] != static_cast<int>(mpcc_prev_sc)) ? 1 : 0);

        if (cost < best_cost - 1.0e-9f
            || (fabsf(cost - best_cost) <= 1.0e-9f
                && (trans < best_trans || (trans == best_trans && idx < best_idx)))) {
            best_cost = cost;
            best_idx = idx;
            best_pred_id = pred_id;
            best_pred_iq = pred_iq;
            best_valpha = valpha;
            best_vbeta = vbeta;
        }
    }

    S_A = static_cast<float>(switch_bits[best_idx][0]);
    S_B = static_cast<float>(switch_bits[best_idx][1]);
    S_C = static_cast<float>(switch_bits[best_idx][2]);
    State_Index = static_cast<float>(best_idx);
    Pred_I_D = rte::Amperes(best_pred_id);
    Pred_I_Q = rte::Amperes(best_pred_iq);
    Cost = cost_track;
    V_Alpha = rte::Volts(best_valpha);
    V_Beta = rte::Volts(best_vbeta);
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
    V_D = rte::Volts(best_valpha * cos_t + best_vbeta * sin_t);
    V_Q = rte::Volts(-best_valpha * sin_t + best_vbeta * cos_t);

    mpcc_prev_sa = S_A;
    mpcc_prev_sb = S_B;
    mpcc_prev_sc = S_C;
    mpcc_u_alpha_prev = best_valpha;
    mpcc_u_beta_prev = best_vbeta;
}
