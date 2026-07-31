/* FCS-MPCC node — Zhang et al. (2017) baseline + improved modes.
 * Technical reference: Y. Zhang, D. Xu, J. Liu, S. Gao, and W. Xu,
 * "Performance Improvement of Model-Predictive Current Control of
 * Permanent Magnet Synchronous Motor Drives," IEEE Transactions on
 * Industry Applications, vol. 53, no. 4, pp. 3683-3695, July/August 2017.
 * DOI: 10.1109/TIA.2017.2690998.
 * Mode: 0=ConventionalOneStep, 1=DelayCompensated, 2=BackEMFCompensated, 3=OptimalDutyCycle
 * Fixed-size state only; no heap allocation in update. */

static float mpcc_prev_sa = 0.0f;
static float mpcc_prev_sb = 0.0f;
static float mpcc_prev_sc = 0.0f;
static float mpcc_u_alpha_prev = 0.0f;
static float mpcc_u_beta_prev = 0.0f;
static float mpcc_id_prev = 0.0f;
static float mpcc_iq_prev = 0.0f;

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
const float id_ref = I_D_Ref.in(au::amperes);
const float iq_ref = I_Q_Ref.in(au::amperes);
const float theta_e = Theta_E;
const float omega_e = Omega_E.in(au::radians / au::seconds);
const bool enable = Enable > 0.5f;

if (!enable || !(ts > 0.0f) || !(vdc > 0.0f) || ld <= 0.0f || lq <= 0.0f) {
    S_A = mpcc_prev_sa;
    S_B = mpcc_prev_sb;
    S_C = mpcc_prev_sc;
} else {
    const float two_thirds = 2.0f / 3.0f;
    const float inv_sqrt3 = 0.57735026919f;
    const float active_vectors[6][2] = {
        {two_thirds, 0.0f},
        {1.0f / 3.0f, inv_sqrt3},
        {-1.0f / 3.0f, inv_sqrt3},
        {-two_thirds, 0.0f},
        {-1.0f / 3.0f, -inv_sqrt3},
        {1.0f / 3.0f, -inv_sqrt3}
    };
    const int switch_bits[8][3] = {
        {0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1},{1,1,1}
    };

    float id_base = id;
    float iq_base = iq;
    if (Mode >= 1.0f) {
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

        const int trans = ((sa != mpcc_prev_sa) ? 1 : 0) + ((sb != mpcc_prev_sb) ? 1 : 0) + ((sc != mpcc_prev_sc) ? 1 : 0);
        const int best_trans = ((switch_bits[best_idx][0] != static_cast<int>(mpcc_prev_sa)) ? 1 : 0)
                             + ((switch_bits[best_idx][1] != static_cast<int>(mpcc_prev_sb)) ? 1 : 0)
                             + ((switch_bits[best_idx][2] != static_cast<int>(mpcc_prev_sc)) ? 1 : 0);

        if (cost < best_cost - 1.0e-9f
            || (fabsf(cost - best_cost) <= 1.0e-9f && (trans < best_trans || (trans == best_trans && idx < best_idx)))) {
            best_cost = cost;
            best_idx = idx;
            best_pred_id = pred_id;
            best_pred_iq = pred_iq;
            best_valpha = valpha;
            best_vbeta = vbeta;
        }
        (void)active_vectors;
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
    mpcc_id_prev = id;
    mpcc_iq_prev = iq;
}
