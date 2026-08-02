/* Predictive current control for three-phase PMSM (Zhang et al., IEEE TIA 2017).
 * Mode: 0=Conventional FCS (full vector / period) — NOT for Gen6 low-L
 *       1=Delay-compensated FCS
 *       2=Back-EMF path (same predictor hook as 1 for now)
 *       3=Deadbeat voltage → SVPWM  ← use this on Gen6 to spin
 *
 * Mode 3 matches the FOC voltage path (Vαβ → Transforms.Svpwm → PwmOut) but
 * replaces PI with a one-step predictive / deadbeat voltage law:
 *   v_dq = R i + ω cross terms + (L/Ts)(i* - i_pred)
 * Tuned for smooth mid-bus Gen6 spin (≈50–100 V): milder deadbeat,
 * current LPF (noise tolerance), longer soft-start. |Id*/Iq*| limited by
 * I_Max only. Holds if Vdc < 40 V. Under voltage limit, prefer Iq (torque).
 */

static float mpcc_prev_sa = 0.0f;
static float mpcc_prev_sb = 0.0f;
static float mpcc_prev_sc = 0.0f;
static float mpcc_u_alpha_prev = 0.0f;
static float mpcc_u_beta_prev = 0.0f;
static float mpcc_id_int = 0.0f;
static float mpcc_iq_int = 0.0f;
static float mpcc_enable_s = 0.0f;
static float mpcc_id_f = 0.0f;
static float mpcc_iq_f = 0.0f;
static float mpcc_i_filt_init = 0.0f;

const float ts = Ts;
float rs = Rs;
float ld = Ld;
float lq = Lq;
float psi_f = PsiF;
if (!(rs > 0.0f)) rs = 0.1f;
if (!(ld > 1.0e-7f)) ld = 0.0001f;
if (!(lq > 1.0e-7f)) lq = 0.0002f;
if (!(psi_f >= 0.0f)) psi_f = 0.01f;
/* Allow calibrated Gen6 flux (~0.07 Wb); old 0.05 clamp starved back-EMF FF. */
if (psi_f > 0.15f) psi_f = 0.15f;

const float i_base = (I_Base > 0.0f) ? I_Base : 10.0f;
const float i_max = (I_Max > 0.0f) ? I_Max : 30.0f;
const float vdc = V_Dc.in(au::volts);
const float id_raw = I_D.in(au::amperes);
const float iq_raw = I_Q.in(au::amperes);
float id_ref = I_D_Ref;
float iq_ref = I_Q_Ref;
const float theta_e = Theta_E;
float omega_e = Omega_E;
/* Cover FOC-class speeds: 2000 rpm @ 10 poles => ωe≈1047 rad/s. */
const float w_max = 3000.0f;
if (omega_e > w_max) omega_e = w_max;
if (omega_e < -w_max) omega_e = -w_max;

const bool enable = Enable;
const int mode_i = (Mode >= 3.0f) ? 3 : ((Mode >= 2.0f) ? 2 : ((Mode >= 1.0f) ? 1 : 0));

const float two_thirds = 2.0f / 3.0f;
const float inv_sqrt3 = 0.57735026919f;
const float sqrt3 = 1.73205080757f;
const int switch_bits[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}
};

/* Clamp |Id*| and |Iq*| to I_Max — no hidden Vdc-based ceiling. */
if (id_ref > i_max) id_ref = i_max;
if (id_ref < -i_max) id_ref = -i_max;
if (iq_ref > i_max) iq_ref = i_max;
if (iq_ref < -i_max) iq_ref = -i_max;

const float cost_track = ((id_ref - id_raw) / i_base) * ((id_ref - id_raw) / i_base)
                       + ((iq_ref - iq_raw) / i_base) * ((iq_ref - iq_raw) / i_base);

/* Hold output unless enabled with a usable DC bus (smooth-spin guard). */
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
    mpcc_prev_sa = 0.0f;
    mpcc_prev_sb = 0.0f;
    mpcc_prev_sc = 0.0f;
    mpcc_u_alpha_prev = 0.0f;
    mpcc_u_beta_prev = 0.0f;
    mpcc_id_int = 0.0f;
    mpcc_iq_int = 0.0f;
    mpcc_enable_s = 0.0f;
    mpcc_id_f = id_raw;
    mpcc_iq_f = iq_raw;
    mpcc_i_filt_init = 0.0f;
} else if (mode_i == 3) {
    /* -------- Deadbeat predictive voltage → SVPWM (Gen6 spin path) -------- */
    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);

    /* LPF measured currents before deadbeat (~500 Hz). Reduces hiss from
     * ADC/quantization noise on low-L Gen6 without killing torque response. */
    if (mpcc_i_filt_init < 0.5f) {
        mpcc_id_f = id_raw;
        mpcc_iq_f = iq_raw;
        mpcc_i_filt_init = 1.0f;
    } else {
        const float fc = 500.0f;
        const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * fc * ts));
        mpcc_id_f += alpha * (id_raw - mpcc_id_f);
        mpcc_iq_f += alpha * (iq_raw - mpcc_iq_f);
    }
    const float id = mpcc_id_f;
    const float iq = mpcc_iq_f;

    /* Delay compensation: predict i(k+1) under the voltage applied last step. */
    const float vd_prev = mpcc_u_alpha_prev * cos_t + mpcc_u_beta_prev * sin_t;
    const float vq_prev = -mpcc_u_alpha_prev * sin_t + mpcc_u_beta_prev * cos_t;
    const float id_p = id + (ts / ld) * (vd_prev - rs * id + omega_e * lq * iq);
    const float iq_p = iq + (ts / lq) * (vq_prev - rs * iq - omega_e * ld * id - omega_e * psi_f);

    /* Longer soft-start so enable does not dump full deadbeat voltage. */
    const float ramp_s = 0.25f;
    mpcc_enable_s += ts;
    if (mpcc_enable_s > ramp_s) mpcc_enable_s = ramp_s;
    const float ramp = mpcc_enable_s / ramp_s;

    /* Soft |i| limit — scale only the feedback/integral terms, keep FF. */
    const float i_meas = sqrtf(id * id + iq * iq);
    float cost = cost_track;
    float fb_scale = ramp;
    if (i_meas > i_max) {
        cost += 1.0e6f;
        if (i_meas > 1.0e-3f) {
            fb_scale *= i_max / i_meas;
        }
    }

    /* Mild deadbeat with filtered currents (less hiss than raw 0.70). */
    const float db_scale = 0.40f;
    const float kp_d = (ld / ts) * db_scale;
    const float kp_q = (lq / ts) * db_scale;
    /* Weak integral — deadbeat already provides most of the action. */
    const float ki = 3.0f;

    const float id_err = id_ref - id_p;
    const float iq_err = iq_ref - iq_p;

    const float v_max = (vdc / sqrt3) * 0.95f;
    const float i_lim_v = v_max * 0.5f; /* keep integral from eating the voltage budget */

    /* Feedforward (plant inversion) — not scaled by overcurrent shrink. */
    const float vd_ff = rs * id_p - omega_e * lq * iq_p;
    const float vq_ff = rs * iq_p + omega_e * ld * id_p + omega_e * psi_f;

    float vd_fb = kp_d * id_err + mpcc_id_int;
    float vq_fb = kp_q * iq_err + mpcc_iq_int;
    vd_fb *= fb_scale;
    vq_fb *= fb_scale;

    float vd_ref = vd_ff * ramp + vd_fb;
    float vq_ref = vq_ff * ramp + vq_fb;

    /* Voltage limit: keep as much Vq (torque) as possible, shed Vd first.
     * Matches FOC field-weakening behaviour at low Vdc. */
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    const bool sat = (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f);
    if (sat) {
        if (fabsf(vq_ref) >= v_max) {
            vq_ref = (vq_ref >= 0.0f) ? v_max : -v_max;
            vd_ref = 0.0f;
        } else {
            const float vd_lim = sqrtf(v_max * v_max - vq_ref * vq_ref);
            if (vd_ref > vd_lim) vd_ref = vd_lim;
            if (vd_ref < -vd_lim) vd_ref = -vd_lim;
        }
        /* Freeze / bleed integrators under saturation (anti-windup). */
        mpcc_id_int *= 0.90f;
        mpcc_iq_int *= 0.90f;
    } else {
        mpcc_id_int += ki * id_err * ts;
        mpcc_iq_int += ki * iq_err * ts;
        if (mpcc_id_int > i_lim_v) mpcc_id_int = i_lim_v;
        if (mpcc_id_int < -i_lim_v) mpcc_id_int = -i_lim_v;
        if (mpcc_iq_int > i_lim_v) mpcc_iq_int = i_lim_v;
        if (mpcc_iq_int < -i_lim_v) mpcc_iq_int = -i_lim_v;
    }

    /* Inverse Park — same convention as Transforms.InversePark. */
    const float valpha_ref = vd_ref * cos_t - vq_ref * sin_t;
    const float vbeta_ref = vd_ref * sin_t + vq_ref * cos_t;

    /* Sector index for telemetry only (PWM comes from SVPWM on Vαβ). */
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

    const float pred_id = id_p + (ts / ld) * (vd_ref - rs * id_p + omega_e * lq * iq_p);
    const float pred_iq = iq_p + (ts / lq) * (vq_ref - rs * iq_p - omega_e * ld * id_p - omega_e * psi_f);

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
    /* -------- Classic FCS enumeration (Modes 0–2) -------- */
    const float id = id_raw;
    const float iq = iq_raw;
    float id_base = id;
    float iq_base = iq;
    if (mode_i >= 1) {
        /* Proper dq delay compensation using last applied αβ voltage. */
        const float cos_t = cosf(theta_e);
        const float sin_t = sinf(theta_e);
        const float vd_prev = mpcc_u_alpha_prev * cos_t + mpcc_u_beta_prev * sin_t;
        const float vq_prev = -mpcc_u_alpha_prev * sin_t + mpcc_u_beta_prev * cos_t;
        id_base = id + (ts / ld) * (vd_prev - rs * id + omega_e * lq * iq);
        iq_base = iq + (ts / lq) * (vq_prev - rs * iq - omega_e * ld * id - omega_e * psi_f);
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
    Cost = best_cost;
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
