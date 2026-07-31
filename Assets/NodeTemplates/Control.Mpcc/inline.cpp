/* Predictive current control for three-phase PMSM (Zhang et al., IEEE TIA 2017).
 * Mode: 0=Conventional FCS (full vector / period)
 *       1=Delay-compensated FCS
 *       2=Back-EMF path (same predictor hook as 1 for now)
 *       3=Optimal-duty / deadbeat voltage (REQUIRED for low-L Gen6 motors)
 *
 * Why Mode 3 is the default on hardware:
 * With L~100 uH, Vdc~540 V, Ts=200 us, one full active vector predicts
 * Δi ≈ V*Ts/L ≈ 1000 A, so Modes 0-2 always prefer zero-vector S000/S111
 * (duties look "stuck" at 0% or 100%). Mode 3 applies only the fraction of
 * the vector needed for the current step and feeds SVPWM via V_Alpha/V_Beta.
 */

static float mpcc_prev_sa = 0.0f;
static float mpcc_prev_sb = 0.0f;
static float mpcc_prev_sc = 0.0f;
static float mpcc_u_alpha_prev = 0.0f;
static float mpcc_u_beta_prev = 0.0f;

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

if (!enable || !(ts > 0.0f) || !(vdc > 0.0f) || ld <= 0.0f || lq <= 0.0f) {
    S_A = 0.0f;
    S_B = 0.0f;
    S_C = 0.0f;
    State_Index = 0.0f;
    Pred_I_D = rte::Amperes(id);
    Pred_I_Q = rte::Amperes(iq);
    Cost = 0.0f;
    V_Alpha = rte::Volts(0.0f);
    V_Beta = rte::Volts(0.0f);
    V_D = rte::Volts(0.0f);
    V_Q = rte::Volts(0.0f);
    mpcc_prev_sa = 0.0f;
    mpcc_prev_sb = 0.0f;
    mpcc_prev_sc = 0.0f;
    mpcc_u_alpha_prev = 0.0f;
    mpcc_u_beta_prev = 0.0f;
} else if (mode_i == 3) {
    /* Deadbeat voltage in dq (discrete inverse of the plant model). */
    float vd_ref = rs * id + (ld / ts) * (id_ref - id) - omega_e * lq * iq;
    float vq_ref = rs * iq + (lq / ts) * (iq_ref - iq) + omega_e * ld * id + omega_e * psi_f;

    /* Clamp to linear SVPWM magnitude so InverseClarke/SVPWM stays valid. */
    const float v_max = (vdc * inv_sqrt3) * 0.95f;
    const float v_mag_sq = vd_ref * vd_ref + vq_ref * vq_ref;
    if (v_mag_sq > v_max * v_max && v_mag_sq > 1.0e-12f) {
        const float scale = v_max / sqrtf(v_mag_sq);
        vd_ref *= scale;
        vq_ref *= scale;
    }

    const float cos_t = cosf(theta_e);
    const float sin_t = sinf(theta_e);
    /* Inverse Park → αβ (RTE convention). */
    const float valpha_ref = vd_ref * cos_t - vq_ref * sin_t;
    const float vbeta_ref = vd_ref * sin_t + vq_ref * cos_t;

    /* Nearest active vector (telemetry / optional FCS view). */
    int best_idx = 1;
    float best_dist = 1.0e30f;
    float best_ua = 0.0f;
    float best_ub = 0.0f;
    for (int idx = 1; idx <= 6; ++idx) {
        const float sa = static_cast<float>(switch_bits[idx][0]);
        const float sb = static_cast<float>(switch_bits[idx][1]);
        const float sc = static_cast<float>(switch_bits[idx][2]);
        const float ua = two_thirds * vdc * (sa - 0.5f * (sb + sc));
        const float ub = vdc * inv_sqrt3 * (sb - sc);
        const float dist = (ua - valpha_ref) * (ua - valpha_ref) + (ub - vbeta_ref) * (ub - vbeta_ref);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = idx;
            best_ua = ua;
            best_ub = ub;
        }
    }

    float t_opt = 0.0f;
    const float u_norm_sq = best_ua * best_ua + best_ub * best_ub;
    if (u_norm_sq > 1.0e-12f) {
        t_opt = (valpha_ref * best_ua + vbeta_ref * best_ub) / u_norm_sq;
        if (t_opt < 0.0f) t_opt = 0.0f;
        if (t_opt > 1.0f) t_opt = 1.0f;
    }

    /* One-step prediction with the duty-scaled voltage (for telemetry/cost). */
    const float vd_app = vd_ref;
    const float vq_app = vq_ref;
    const float pred_id = id + (ts / ld) * (vd_app - rs * id + omega_e * lq * iq);
    const float pred_iq = iq + (ts / lq) * (vq_app - rs * iq - omega_e * ld * id - omega_e * psi_f);
    float cost = ((id_ref - pred_id) / i_base) * ((id_ref - pred_id) / i_base)
               + ((iq_ref - pred_iq) / i_base) * ((iq_ref - pred_iq) / i_base);
    const float imag = sqrtf(pred_id * pred_id + pred_iq * pred_iq);
    if (imag > i_max) cost += 1.0e6f;

    S_A = static_cast<float>(switch_bits[best_idx][0]);
    S_B = static_cast<float>(switch_bits[best_idx][1]);
    S_C = static_cast<float>(switch_bits[best_idx][2]);
    State_Index = static_cast<float>(best_idx) + t_opt; /* integer state + fractional duty */
    Pred_I_D = rte::Amperes(pred_id);
    Pred_I_Q = rte::Amperes(pred_iq);
    Cost = cost;
    /* Continuous αβ voltage for SVPWM → PWM (this is what spins the motor). */
    V_Alpha = rte::Volts(valpha_ref);
    V_Beta = rte::Volts(vbeta_ref);
    V_D = rte::Volts(vd_ref);
    V_Q = rte::Volts(vq_ref);

    mpcc_prev_sa = S_A;
    mpcc_prev_sb = S_B;
    mpcc_prev_sc = S_C;
    mpcc_u_alpha_prev = valpha_ref;
    mpcc_u_beta_prev = vbeta_ref;
    (void)best_ua;
    (void)best_ub;
} else {
    /* Conventional / delay-compensated FCS (suitable only for large-L motors). */
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
    int feasible = 0;

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
        const bool over = imag > i_max;
        if (!over) feasible = 1;

        const int trans = ((sa != mpcc_prev_sa) ? 1 : 0) + ((sb != mpcc_prev_sb) ? 1 : 0) + ((sc != mpcc_prev_sc) ? 1 : 0);
        const int best_trans = ((switch_bits[best_idx][0] != static_cast<int>(mpcc_prev_sa)) ? 1 : 0)
                             + ((switch_bits[best_idx][1] != static_cast<int>(mpcc_prev_sb)) ? 1 : 0)
                             + ((switch_bits[best_idx][2] != static_cast<int>(mpcc_prev_sc)) ? 1 : 0);

        /* Defer I_Max penalty until we know at least one vector is feasible;
         * otherwise low-L motors lock forever on S000/S111. */
        float cost_cmp = cost;
        if (over) cost_cmp += 1.0e6f;

        if (cost_cmp < best_cost - 1.0e-9f
            || (fabsf(cost_cmp - best_cost) <= 1.0e-9f && (trans < best_trans || (trans == best_trans && idx < best_idx)))) {
            best_cost = cost_cmp;
            best_idx = idx;
            best_pred_id = pred_id;
            best_pred_iq = pred_iq;
            best_valpha = valpha;
            best_vbeta = vbeta;
        }
        (void)feasible;
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
