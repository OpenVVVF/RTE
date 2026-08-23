#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Encoderless induction-motor inductance measurement via V/Hz spin.
 *
 * Spins the motor in open-loop scalar (V/Hz) mode at a fixed electrical
 * frequency, auto-hunts for a modulation index that produces a usable
 * magnetizing current, then sweeps around that operating point.  At each
 * steady point the steady-state stator current is correlated against the known
 * voltage angle (lock-in detection).  For a freely spinning motor slip is near
 * zero, so the per-phase impedance is approximately:
 *
 *     Z = Rs + j * omega_e * Ls
 *
 * from which the total stator inductance is:
 *
 *     Ls = X_L / omega_e
 *
 * Because the magnetizing current changes with V/Hz, the sweep produces a
 * saturation curve Ls(I).  No encoder is required; the shaft must simply be
 * free to spin (no load).
 */
class InductionVHzCalibrator {
public:
    static constexpr int MAX_POINTS = 16;

    InductionVHzCalibrator() = default;

    /**
     * @brief Start the V/Hz inductance sweep.
     *
     * Requires: valid resistance calibration, DC bus present, no active
     * Critical/High faults, motor stopped, no other calibration active.
     *
     * The routine first auto-hunts for a modulation index that produces a
     * magnetizing current in the target window while the rotor stays in sync.
     * It then measures a short saturation sweep around that point.
     *
     * @param freq_hz            Electrical fundamental frequency [Hz].  Lower
     *                           frequencies give larger magnetizing current for
     *                           the same voltage, which helps noisy current
     *                           sensors (try 5..20 Hz).
     * @param max_modulation     Maximum SVPWM modulation index to allow during
     *                           the hunt [0..1.15].
     * @param n_points           Number of points in the final saturation sweep
     *                           around the found operating point (>= 3).
     * @param settle_ms          Time to wait after each final sweep step before
     *                           measuring.
     * @param measure_ms         Time to correlate current over at each final
     *                           sweep step; use several cycles for good noise
     *                           rejection.
     * @param current_limit_a    Hard abort if phase current exceeds this.
     * @param delta_connected    True if the motor is delta-connected; line
     *                           current is divided by sqrt(3) to obtain phase
     *                           current for the impedance calculation.
     */
    bool start(float freq_hz = 10.0f,
               float max_modulation = 0.50f,
               int n_points = 5,
               uint32_t settle_ms = 1000U,
               uint32_t measure_ms = 2000U,
               float current_limit_a = 80.0f,
               bool delta_connected = true);
    void stop();

    /** Main-loop state machine; call every iteration. */
    void update();

    bool isActive() const;
    bool isDone() const { return m_state == State::DONE; }
    bool isFailed() const { return m_state == State::FAIL; }
    const char* stateName() const;

    int pointCount() const { return m_n_results; }
    float currentPoint(int i) const;
    float modulationPoint(int i) const;
    float lsPoint(int i) const;

    static InductionVHzCalibrator& instance();

private:
    enum class State {
        IDLE,
        START,
        HUNT_START,
        HUNT_SETTLE,
        HUNT_MEASURE,
        HUNT_CHECK,
        SWEEP_START,
        RAMP,
        SETTLE,
        MEASURE,
        NEXT,
        FINISH,
        DONE,
        FAIL
    };

    void enterState(State s);
    void fail(const char* reason_fmt, ...);
    void shutdown();
    void resetAccumulators();

    bool sampleCurrents(float& iu, float& iv, float& iw);
    float maxAbs3(float a, float b, float c) const;
    void accumulateLockIn(float iu, float iv, float iw, float theta);
    bool computePoint(float vdc, float& out_i_a, float& out_ls_h,
                      float& out_phi_deg) const;

    State m_state = State::IDLE;
    uint32_t m_state_enter_ms = 0;
    char m_fail_reason[96] = {0};

    /* Configuration. */
    float m_freq_hz = 10.0f;
    float m_max_mod = 0.50f;
    int m_n_points = 5;
    uint32_t m_settle_ms = 1000U;
    uint32_t measure_ms_ = 2000U;
    float m_current_limit_a = 80.0f;
    bool m_delta = true;
    float m_i_scale = 1.0f;  /**< 1/sqrt(3) for delta, 1 for wye. */

    /* Auto-hunt configuration. */
    float m_hunt_i_min = 5.0f;     /**< Minimum useful phase current [A]. */
    float m_hunt_i_max = 15.0f;    /**< Maximum useful phase current [A]. */
    float m_hunt_mod_step = 0.02f; /**< Modulation step during hunt. */
    uint32_t m_hunt_settle_ms = 200U;
    uint32_t m_hunt_measure_ms = 500U;

    /* Hunt state. */
    float m_hunt_mod = 0.0f;
    float m_base_mod = 0.0f;
    float m_hunt_i_a = 0.0f;
    float m_hunt_phi_deg = 0.0f;

    /* Sweep state. */
    int m_point = 0;
    float m_mod_step = 0.0f;
    float m_target_mod = 0.0f;

    /* Lock-in accumulators.  We correlate phase currents against the known
     * voltage angle to extract the fundamental amplitude and phase, rejecting
     * ADC noise and DC offset. */
    uint32_t m_samples = 0;
    float m_sum_iu_sin = 0.0f;
    float m_sum_iu_cos = 0.0f;
    float m_sum_iv_sin = 0.0f;
    float m_sum_iv_cos = 0.0f;
    float m_sum_iw_sin = 0.0f;
    float m_sum_iw_cos = 0.0f;
    float m_peak_i = 0.0f;

    /* Results. */
    int m_n_results = 0;
    float m_i_a[MAX_POINTS] = {};
    float m_mod[MAX_POINTS] = {};
    float m_ls_h[MAX_POINTS] = {};

    static constexpr float PI_F = 3.14159265358979323846f;
};

InductionVHzCalibrator& inductionVHzCalibrator();

} // namespace Inverter
