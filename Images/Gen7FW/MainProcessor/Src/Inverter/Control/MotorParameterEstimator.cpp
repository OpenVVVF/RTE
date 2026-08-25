#include "Inverter/Control/MotorParameterEstimator.h"
#include "Inverter/Control/CurrentObserver.h"

#include <cmath>

namespace Inverter {

static MotorParameterEstimator s_instance;

MotorParameterEstimator& motorParameterEstimator() {
    return s_instance;
}

void MotorParameterEstimator::init(float r_seed_ohm, float l_seed_henry,
                                   float forgetting) {
    m_theta[0] = r_seed_ohm;
    m_theta[1] = l_seed_henry;
    m_lambda = forgetting;
    reset();
}

void MotorParameterEstimator::reset() {
    m_p[0][0] = 1.0f;
    m_p[0][1] = 0.0f;
    m_p[1][0] = 0.0f;
    m_p[1][1] = 1.0f;
    m_update_count = 0;
    m_converged = false;
}

void MotorParameterEstimator::update(float valpha_v, float vbeta_v,
                                     float e_alpha_v, float e_beta_v,
                                     float i_alpha_a, float i_beta_a,
                                     float diudt_a_per_s, float divdt_a_per_s,
                                     float observer_confidence) {
    if (observer_confidence < m_min_confidence) {
        return;
    }

    /* Convert U/V slopes to alpha/beta slopes. */
    const float di_alpha = diudt_a_per_s;
    const float di_beta  = (diudt_a_per_s + 2.0f * divdt_a_per_s) * 0.57735026919f;

    const float excitation = std::sqrt(di_alpha * di_alpha + di_beta * di_beta);
    if (excitation < m_min_excitation) {
        return;
    }

    /* Residual voltages after removing back-EMF. */
    const float y_alpha = valpha_v - e_alpha_v;
    const float y_beta  = vbeta_v  - e_beta_v;

    /* RLS regression vector: phi = [i, di/dt].  We have two equations
     * (alpha and beta); update sequentially. */
    const float phi[2][2] = {
        {i_alpha_a, di_alpha},
        {i_beta_a,  di_beta},
    };
    const float y[2] = {y_alpha, y_beta};

    for (int eq = 0; eq < 2; ++eq) {
        const float p00 = m_p[0][0], p01 = m_p[0][1];
        const float p10 = m_p[1][0], p11 = m_p[1][1];
        const float ph0 = phi[eq][0], ph1 = phi[eq][1];

        /* denominator = lambda + phi^T * P * phi */
        const float den = m_lambda +
            ph0 * (p00 * ph0 + p01 * ph1) +
            ph1 * (p10 * ph0 + p11 * ph1);
        if (den <= 1.0e-12f) {
            continue;
        }

        /* K = P * phi / den */
        const float k0 = (p00 * ph0 + p01 * ph1) / den;
        const float k1 = (p10 * ph0 + p11 * ph1) / den;

        /* theta += K * (y - phi^T * theta) */
        const float pred = ph0 * m_theta[0] + ph1 * m_theta[1];
        const float err  = y[eq] - pred;
        m_theta[0] += k0 * err;
        m_theta[1] += k1 * err;

        /* P = (P - K * phi^T * P) / lambda */
        const float kp00 = k0 * ph0 * p00 + k0 * ph1 * p10;
        const float kp01 = k0 * ph0 * p01 + k0 * ph1 * p11;
        const float kp10 = k1 * ph0 * p00 + k1 * ph1 * p10;
        const float kp11 = k1 * ph0 * p01 + k1 * ph1 * p11;

        m_p[0][0] = (p00 - kp00) / m_lambda;
        m_p[0][1] = (p01 - kp01) / m_lambda;
        m_p[1][0] = (p10 - kp10) / m_lambda;
        m_p[1][1] = (p11 - kp11) / m_lambda;

        ++m_update_count;
    }

    /* Convergence: covariance trace below a threshold. */
    const float trace = m_p[0][0] + m_p[1][1];
    if (trace < 0.1f && m_update_count > 100U) {
        m_converged = true;
    }

    /* Push the estimates into the observer whenever they look sane. */
    if (m_update_count > 10U) {
        currentObserver().updateEstimatedParameters(m_theta[0], m_theta[1]);
    }
}

} // namespace Inverter
