#pragma once

#include "Inverter/Calibration/Common/CalibrationHardware.h"

#include <cstdint>

namespace Inverter {

/**
 * @brief Offline induction-motor parameter estimation.
 *
 * Measures the two standstill parameters that an induction FOC needs:
 *   - sigma_Ls: stator transient (leakage) inductance [H], from a high-frequency
 *     locked-rotor AC injection.
 *   - tau_r: rotor time constant Lr/Rr' [ms], from a DC fluxing + stator-short
 *     current decay test.
 *
 * The calibrator deliberately does NOT require an encoder.  It uses the
 * open-loop SPWM generator for the AC injection and direct PWM parking for the
 * DC fluxing/decay.  If an encoder is present, a small movement guard aborts
 * the run if the rotor shifts during the DC phase.
 *
 * Magnetizing inductance Lm cannot be determined from standstill tests alone,
 * so it is accepted as an optional estimate.  When provided, Lr, Rr' and the
 * leakage inductance are derived under the standard assumption Lls = Llr'.
 */
class InductionMotorCalibrator {
public:
    static constexpr int MAX_DECAY_SAMPLES = 200;

    InductionMotorCalibrator() = default;

    /**
     * @brief Start the calibration.
     *
     * Requires: valid resistance calibration, DC bus present, no active
     * Critical/High faults, motor stopped, no other calibration active.
     *
     * @param max_flux_current_a    Target DC fluxing current [A].
     * @param ac_voltage_pct        AC injection peak line-to-neutral voltage as
     *                              a percentage of Vdc/2 (e.g., 5 = 5%).
     * @param ac_freq_hz            AC injection frequency [Hz].  Must be well
     *                              above 1/tau_r so the rotor cage does not follow.
     * @param fluxing_time_ms       Time to hold DC flux before the decay [ms].
     * @param decay_sample_time_ms  Maximum length of the decay recording [ms]
     *                              (<= 2000 ms to fit the recording buffer).
     * @param lm_estimate_henry     Optional user-supplied magnetizing inductance.
     *                              If <= 0, derived parameters are left unset.
     */
    bool start(float max_flux_current_a = 20.0f,
               float ac_voltage_pct = 5.0f,
               float ac_freq_hz = 100.0f,
               uint32_t fluxing_time_ms = 3000U,
               uint32_t decay_sample_time_ms = 2000U,
               float lm_estimate_henry = 0.0f);
    void stop();

    /** Main-loop state machine; call every iteration. */
    void update();

    bool isActive() const;
    bool isDone() const { return m_state == State::DONE; }
    bool isFailed() const { return m_state == State::FAIL; }
    const char* stateName() const;

    /* Measured headline results. */
    float sigmaLsHenry() const { return m_sigma_ls_h; }
    float rotorTauMs() const { return m_rotor_tau_ms; }

    /* Derived results (valid only if an Lm estimate was supplied). */
    float lmHenry() const { return m_lm_h; }
    float lrHenry() const { return m_lr_h; }
    float rrOhm() const { return m_rr_ohm; }
    float lLeakHenry() const { return m_l_leak_h; }

    static InductionMotorCalibrator& instance();

private:
    enum class State {
        IDLE,
        ENABLE,
        WAIT_READY,
        SIGMA_LS_SETTLE,
        SIGMA_LS_MEASURE,
        TAU_R_FLUXING,
        TAU_R_SETTLE,
        TAU_R_DECAY,
        COMPUTE,
        DONE,
        FAIL
    };

    void enterState(State s);
    void fail(const char* reason_fmt, ...);
    void shutdown();

    void sampleCurrents(float& iu, float& iv, float& iw);
    bool checkOvercurrent(float iu, float iv, float iw);
    bool checkEncoderMovement();
    void updateFluxPi(float i_meas_a);

    bool runSigmaLsSettle();
    bool runSigmaLsMeasure();
    bool runTauRFluxing();
    bool runTauRSettle();
    bool runTauRDecay();
    bool runCompute();

    State m_state = State::IDLE;
    uint32_t m_state_enter_ms = 0;
    char m_fail_reason[96] = {0};

    CalibrationHardware m_hw;

    /* Configuration. */
    float m_max_flux_current_a = 20.0f;
    float m_ac_voltage_pct = 5.0f;
    float m_ac_freq_hz = 100.0f;
    uint32_t m_fluxing_time_ms = 3000U;
    uint32_t m_decay_sample_time_ms = 3000U;
    float m_lm_estimate_h = 0.0f;

    /* AC injection state. */
    float m_ac_mod_index = 0.0f;
    uint32_t m_spwm_start_cycles = 0;

    /* Sigma-Ls measurement. */
    float m_sigma_i_peak = 0.0f;
    uint32_t m_sigma_samples = 0;

    /* Rotor fluxing PI (modulation index -> current). */
    float m_flux_mod_index = 0.0f;
    float m_flux_integral = 0.0f;
    uint32_t m_flux_pi_last_ms = 0;

    /* Decay recording. */
    int m_decay_n = 0;
    uint32_t m_last_decay_ms = 0;
    float m_decay_t_ms[MAX_DECAY_SAMPLES];
    float m_decay_i_a[MAX_DECAY_SAMPLES];

    /* Encoder guard (optional). */
    bool m_encoder_guard = false;
    float m_encoder_start_angle = 0.0f;

    /* Results. */
    float m_sigma_ls_h = 0.0f;
    float m_rotor_tau_ms = 0.0f;
    float m_lm_h = 0.0f;
    float m_lr_h = 0.0f;
    float m_rr_ohm = 0.0f;
    float m_l_leak_h = 0.0f;

    static constexpr float FLUX_PI_KP = 0.002f;
    static constexpr float FLUX_PI_KI = 0.02f;
    static constexpr float FLUX_PI_MIN_MOD = 0.0f;
    static constexpr float FLUX_PI_MAX_MOD = 0.25f;
    static constexpr uint32_t SETTLE_MS = 500U;
    static constexpr uint32_t AC_MEASURE_CYCLES = 50U;
    static constexpr float OVERCURRENT_A = 200.0f;
    static constexpr float ENCODER_MOVE_DEG = 5.0f;
    static constexpr float MIN_DECAY_FIT_TAU_MS = 5.0f;
};

InductionMotorCalibrator& inductionMotorCalibrator();

} // namespace Inverter
