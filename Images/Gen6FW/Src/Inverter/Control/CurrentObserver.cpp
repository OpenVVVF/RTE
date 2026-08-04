#include "Inverter/Control/CurrentObserver.h"

#include <cmath>

namespace Inverter {

static CurrentObserver s_instance;

CurrentObserver& currentObserver() {
    return s_instance;
}

void CurrentObserver::setMotorParameters(float r_ohm, float l_henry,
                                         float flux_linkage_wb,
                                         float pole_pairs) {
    m_r = r_ohm;
    m_l = l_henry;
    m_flux = flux_linkage_wb;
    m_pole_pairs = pole_pairs;
}

void CurrentObserver::predict(float valpha_v, float vbeta_v,
                              float theta_elec_rad, float dt_s) {
    if (dt_s <= 0.0f || m_l <= 0.0f) {
        return;
    }

    /* Electrical back-EMF from rotor position and speed.  When flux linkage
     * is not calibrated we rely on the disturbance-estimator part of the
     * observer to track e_alpha/e_beta. */
    float e_alpha = m_e_alpha;
    float e_beta  = m_e_beta;
    if (m_flux > 0.0f) {
        const float sin_th = std::sin(theta_elec_rad);
        const float cos_th = std::cos(theta_elec_rad);
        e_alpha = -m_flux * sin_th;
        e_beta  =  m_flux * cos_th;
    }

    const float Ts_over_L = dt_s / m_l;
    m_i_alpha += Ts_over_L * (valpha_v - m_r * m_i_alpha - e_alpha);
    m_i_beta  += Ts_over_L * (vbeta_v  - m_r * m_i_beta  - e_beta);

    /* Decay confidence if no correction has arrived. */
    m_confidence *= 0.95f;
}

void CurrentObserver::correct(float iu_meas_a, float iv_meas_a,
                              float diudt_a_per_s, float divdt_a_per_s,
                              uint32_t t_us) {
    /* Convert two-phase (U,V) to stationary alpha/beta. */
    const float i_alpha_meas = iu_meas_a;
    const float i_beta_meas  = (iu_meas_a + 2.0f * iv_meas_a) * 0.57735026919f;

    const float err_alpha = i_alpha_meas - m_i_alpha;
    const float err_beta  = i_beta_meas  - m_i_beta;

    m_i_alpha += m_k_i * err_alpha;
    m_i_beta  += m_k_i * err_beta;

    m_e_alpha += m_k_e * err_alpha;
    m_e_beta  += m_k_e * err_beta;

    m_diudt_meas = diudt_a_per_s;
    m_divdt_meas = divdt_a_per_s;
    m_last_correction_us = t_us;
    m_confidence = 1.0f;
}

void CurrentObserver::getPhaseCurrents(float& iu_a, float& iv_a, float& iw_a) const {
    /* Inverse Clarke: alpha/beta -> U,V,W. */
    iu_a = m_i_alpha;
    iv_a = -0.5f * m_i_alpha + 0.86602540378f * m_i_beta;
    iw_a = -0.5f * m_i_alpha - 0.86602540378f * m_i_beta;
}

void CurrentObserver::reset() {
    m_i_alpha = 0.0f;
    m_i_beta  = 0.0f;
    m_e_alpha = 0.0f;
    m_e_beta  = 0.0f;
    m_confidence = 0.0f;
    m_diudt_meas = 0.0f;
    m_divdt_meas = 0.0f;
    m_last_correction_us = 0;
}

} // namespace Inverter
