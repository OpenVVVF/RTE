#pragma once

/* Faithful host port of Gen6FW's Inverter::CurrentObserver
 * (Images/Gen6FW/Inc/Inverter/Control/CurrentObserver.h and
 * Images/Gen6FW/Src/Inverter/Control/CurrentObserver.cpp).  The math and
 * state layout are identical to firmware; only the namespace and the
 * singleton accessor name (GlobalCurrentObserver, matching HostSim's other
 * base-image singletons) differ.  Warnings/errors in behavior should be
 * fixed upstream first and re-ported. */

#include <cstdint>

namespace hostsim {

/* Current + back-EMF observer for modulation-agnostic current reconstruction.
 *
 * Runs in the stationary alpha/beta frame.  Predicts phase currents from the
 * applied voltage vector and corrects them when clean micro-burst samples are
 * available.  The FOC loop consumes the observer output instead of raw ADC
 * samples, so zero-vector periods, short SHE pulses, and noisy switching
 * edges do not destabilize the control loop.
 *
 * Model (per axis):
 *   v = R*i + L*di/dt + e
 * Observer:
 *   i_hat[k+1] = i_hat[k] + Ts/L * (v - R*i_hat[k] - e_hat[k]) + K_i*(i_meas - i_hat[k])
 *   e_hat[k+1] = e_hat[k] + K_e*(i_meas - i_hat[k])
 */
class CurrentObserver {
public:
    CurrentObserver() = default;

    /* Set motor parameters used by the predictor. */
    void setMotorParameters(float r_ohm, float l_henry, float flux_linkage_wb,
                            float pole_pairs);

    /* Run one prediction step using the applied voltage vector.  Call once
     * per control ISR with the voltage that will be applied for the next dt
     * seconds. */
    void predict(float valpha_v, float vbeta_v, float theta_elec_rad, float dt_s);

    /* Correct the observer with a measured two-phase current pair.  The
     * slopes diu/dt and div/dt are stored for the RLS estimator but are not
     * used by the observer correction itself. */
    void correct(float iu_meas_a, float iv_meas_a,
                 float diudt_a_per_s, float divdt_a_per_s,
                 uint32_t t_us);

    /* Get the observer-estimated phase currents. */
    void getPhaseCurrents(float& iu_a, float& iv_a, float& iw_a) const;

    /* Observer confidence [0, 1].  Decays without corrections. */
    float confidence() const { return m_confidence; }

    /* Current estimate in stationary alpha/beta frame. */
    float iAlpha() const { return m_i_alpha; }
    float iBeta()  const { return m_i_beta; }

    /* Back-EMF estimate in stationary alpha/beta frame. */
    float eAlpha() const { return m_e_alpha; }
    float eBeta()  const { return m_e_beta; }

    /* Latest measured slopes from the last micro-burst correction. */
    float diudtMeasured() const { return m_diudt_meas; }
    float divdtMeasured() const { return m_divdt_meas; }
    uint32_t lastCorrectionUs() const { return m_last_correction_us; }

    /* Current motor parameters (for diagnostics / seeding checks). */
    float rOhm() const { return m_r; }
    float lHenry() const { return m_l; }

    /* Update R and L used by the predictor (e.g. from RLS estimator). */
    void updateEstimatedParameters(float r_ohm, float l_henry) {
        m_r = r_ohm;
        m_l = l_henry;
    }

    /* Reset observer state. */
    void reset();

private:
    /* Motor parameters (defaults identical to the Gen6 firmware). */
    float m_r = 0.0145f;       /* Phase resistance [ohm]. */
    float m_l = 40.0e-6f;      /* Phase inductance [H]. */
    float m_flux = 0.0f;       /* PM flux linkage [Wb]. */
    float m_pole_pairs = 5.0f;

    /* Observer state in stationary alpha/beta. */
    float m_i_alpha = 0.0f;
    float m_i_beta  = 0.0f;
    float m_e_alpha = 0.0f;
    float m_e_beta  = 0.0f;

    /* Correction gains. */
    float m_k_i = 0.25f;
    float m_k_e = 0.05f;

    /* Confidence decays each predict step without correction. */
    float m_confidence = 0.0f;

    /* Latest measured slopes (for RLS). */
    float m_diudt_meas = 0.0f;
    float m_divdt_meas = 0.0f;
    uint32_t m_last_correction_us = 0;
};

/* Global observer instance behind the platform_observer_* API, mirroring
 * Gen6's currentObserver() singleton. */
CurrentObserver& GlobalCurrentObserver();

} // namespace hostsim
