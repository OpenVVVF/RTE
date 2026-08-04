#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Online RLS estimator for motor phase resistance and inductance.
 *
 * Uses the voltage equation during active vectors:
 *   v - e = R*i + L*di/dt
 *
 * Each micro-burst provides one equation with the two unknowns.  Recursive
 * least squares with a forgetting factor tracks R and L as the motor heats
 * up or saturates.  Updates are gated on sufficient excitation and observer
 * confidence so garbage data does not corrupt the estimates.
 */
class MotorParameterEstimator {
public:
    MotorParameterEstimator() = default;

    /**
     * @brief Initialize with seed values and forgetting factor.
     */
    void init(float r_seed_ohm, float l_seed_henry, float forgetting = 0.99f);

    /**
     * @brief Process one micro-burst measurement.
     *
     * @param valpha_v     Applied alpha voltage [V].
     * @param vbeta_v      Applied beta voltage [V].
     * @param e_alpha_v    Observer-estimated back-EMF alpha [V].
     * @param e_beta_v     Observer-estimated back-EMF beta [V].
     * @param i_alpha_a    Observer-estimated alpha current [A].
     * @param i_beta_a     Observer-estimated beta current [A].
     * @param diudt_a_per_s  Measured U current slope [A/s].
     * @param divdt_a_per_s  Measured V current slope [A/s].
     * @param observer_confidence  Observer confidence [0,1].
     */
    void update(float valpha_v, float vbeta_v,
                float e_alpha_v, float e_beta_v,
                float i_alpha_a, float i_beta_a,
                float diudt_a_per_s, float divdt_a_per_s,
                float observer_confidence);

    float estimatedR() const { return m_theta[0]; }
    float estimatedL() const { return m_theta[1]; }

    /**
     * @brief true once the covariance has shrunk enough to trust the estimates.
     */
    bool converged() const { return m_converged; }

    /**
     * @brief Number of gated updates applied so far.
     */
    uint32_t updateCount() const { return m_update_count; }

    /**
     * @brief Reset the estimator.
     */
    void reset();

private:
    /* theta = [R, L]. */
    float m_theta[2] = {0.0145f, 40.0e-6f};

    /* 2x2 covariance matrix. */
    float m_p[2][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};

    float m_lambda = 0.99f;
    float m_min_excitation = 100.0f;   /**< Minimum |di/dt| [A/s] to update. */
    float m_min_confidence = 0.5f;

    uint32_t m_update_count = 0;
    bool m_converged = false;
};

/**
 * @brief Global estimator instance.
 */
MotorParameterEstimator& motorParameterEstimator();

} // namespace Inverter
