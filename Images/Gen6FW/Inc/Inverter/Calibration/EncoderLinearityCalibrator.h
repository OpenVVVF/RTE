#pragma once

#include "Inverter/Calibration/Common/BreakawayFinder.h"
#include "Inverter/Calibration/Common/CalibrationHardware.h"
#include "Inverter/Calibration/Common/EncoderTracker.h"

#include <cstdint>

namespace Inverter {

/**
 * @brief Measure encoder angle nonlinearity (INL) with a locked rotor.
 *
 * Uses the same BreakawayFinder as the offset cal to find the minimum
 * modulation that breaks the rotor loose, then steps the stator field angle
 * in small mechanical increments and waits for the encoder to settle before
 * taking each sample.  This avoids slip, keeps current at the minimum needed
 * for lock, and bins the residual (encoder angle - field angle) by encoder
 * angle for a DFT harmonic report.
 *
 * With dual_pass enabled the sweep runs forward and then back over the same
 * positions and the two per-bin means are averaged.  Static torque-angle lag
 * (cogging/load) flips sign with approach direction and cancels, while true
 * encoder nonlinearity does not — this separates sensor INL from lag, which a
 * single-direction static sweep cannot.
 *
 * The bin buffers live in the shared calibration scratch memory (AXISRAM).
 */
class EncoderLinearityCalibrator {
public:
    EncoderLinearityCalibrator() = default;

    /**
     * @brief Start a linearity measurement.
     *
     * @param pole_count            Total rotor pole count.
     * @param encoder_cycles_per_rev Encoder electrical cycles per mechanical rev.
     * @param rotate_mod            Modulation index for the locked steps.
     *                              If <= 0, BreakawayFinder is used.
     * @param revolutions           Number of mechanical revolutions to average.
     * @param step_size_mech_deg    Mechanical angle step between samples [deg].
     * @return true if the calibrator started.
     */
    bool start(float pole_count, float encoder_cycles_per_rev,
               float rotate_mod = 0.0f, float revolutions = 3.0f,
               float step_size_mech_deg = 0.0f, bool dual_pass = true);

    /** Non-blocking state-machine update; call at ~100 Hz. */
    void update();

    /** Abort a running measurement. */
    void stop();

    bool isActive() const {
        return m_state != State::IDLE && m_state != State::DONE &&
               m_state != State::FAIL;
    }
    bool isDone() const { return m_state == State::DONE; }
    bool isFailed() const { return m_state == State::FAIL; }

    const char* stateName() const;

    /** Number of valid bins with samples after a completed run. */
    int validBinCount() const { return m_valid_bins; }

    /** Number of bins sampled in BOTH directions after a dual-pass run. */
    int pairedBinCount() const { return m_paired_bins; }

    /** RMS of the direction-dependent (torque-lag) component [deg]. */
    float lagRmsDeg() const { return m_lag_rms_deg; }

    /** Residual RMS in degrees after the last run. */
    float residualRmsDeg() const { return m_residual_rms_deg; }

    /** Peak-to-peak residual in degrees after the last run. */
    float residualPpDeg() const { return m_residual_pp_deg; }

    /** Amplitude of harmonic k (1..16) in degrees. 0 if not run. */
    float harmonicAmplitude(int harmonic) const;

    static EncoderLinearityCalibrator& instance();

    enum class State {
        IDLE,
        HW_INIT,
        FIND_VOLTAGE,
        BREAKAWAY_SETTLE,
        STEP,
        WAIT_SETTLE,
        MEASURE,
        ANALYZE,
        DONE,
        FAIL
    };

private:
    struct Bin {
        double sum = 0.0;
        uint32_t count = 0;
    };

    void enterState(State state);
    void fail(const char* reason_fmt, ...);
    void restoreHardware();

    float fieldMechanicalAngle() const;
    void commandFieldAngle(float field_mech_deg);
    void updateHoldThrottle(uint32_t now_ms);
    static float wrap180(float angle_deg);
    static int encoderBin(float encoder_deg);

    void resetBins();
    void updateEncoderTracking();
    void accumulateSample();
    void analyze();

    static constexpr int BIN_COUNT = 1024;
    static constexpr int MAX_HARMONIC = 16;
    static constexpr float DEFAULT_ROTATE_MOD = 0.0f;  /* use breakaway */
    static constexpr float DEFAULT_STEP_SIZE_MECH_DEG = 0.5f;
    static constexpr float BREAKAWAY_MARGIN = 1.00f;   /* rotation = breakaway (no headroom) */
    static constexpr float MAX_ROTATE_MOD = 0.35f;
    static constexpr float ROTATE_FREQUENCY_HZ = 1.0f;  /* for breakaway only */
    /* The settle check only needs to prove the rotor STOPPED — the residual is
     * computed from the actual encoder reading, not the assumption that the
     * rotor reached the commanded angle.  Keep this generous enough to ride
     * over cogging-hunting oscillation. */
    static constexpr float SETTLE_POS_THRESHOLD_DEG = 0.50f;
    static constexpr uint32_t SETTLE_DURATION_MS = 100U;
    static constexpr uint32_t STEP_TIMEOUT_MS = 2000U;
    static constexpr uint32_t BREAKAWAY_SETTLE_MS = 500U;
    static constexpr uint32_t HW_INIT_TIMEOUT_MS = 10000U;
    static constexpr uint32_t OVERALL_TIMEOUT_MS = 600000U;  /* 10 min max */

    /* Overcurrent safety.  BreakawayFinder has no current throttle (unlike
     * CurrentLimitedRamp), and a static holding vector draws V/R continuously,
     * so without a hard abort a stuck rotor cooks the stage.  This is a pure
     * safety ceiling, not a control target: anything sustained above it kills
     * the run and drops the PWM immediately. */
    static constexpr float OVERCURRENT_ABORT_A = 50.0f;
    static constexpr uint32_t OVERCURRENT_SUSTAIN_MS = 250U;

    /* Static-hold current target.  A stationary field vector draws DC through
     * the windings, so the breakaway-derived modulation is only a ceiling;
     * the applied hold mod is throttled to keep phase current near this
     * target (same ballpark as the rotating offset cal). */
    static constexpr float HOLD_CURRENT_TARGET_A = 25.0f;
    static constexpr float HOLD_MOD_FLOOR = 0.005f;

    State m_state = State::IDLE;
    CalibrationHardware m_hw;
    BreakawayFinder m_breakaway;
    EncoderTracker m_tracker;

    float m_pole_pairs = 0.0f;
    float m_encoder_cycles_per_rev = 1.0f;
    float m_rotate_mod = DEFAULT_ROTATE_MOD;
    float m_revolutions = 3.0f;
    float m_step_size_mech_deg = DEFAULT_STEP_SIZE_MECH_DEG;

    uint32_t m_state_start_ms = 0;
    uint32_t m_last_dbg_ms = 0;
    uint32_t m_oc_start_ms = 0;
    float m_last_current_a = 0.0f;

    /* Current-targeted static hold. */
    float m_hold_mod = 0.0f;
    uint32_t m_last_throttle_ms = 0;
    float m_target_field_mech = 0.0f;
    float m_prev_encoder_deg = 0.0f;
    float m_encoder_unwrapped = 0.0f;

    /* Step-and-settle tracking. */
    int m_step_index = 0;
    int m_total_steps = 0;
    uint32_t m_settle_start_ms = 0;
    float m_settle_ref_encoder = 0.0f;
    bool m_settled = false;

    Bin* m_bins = nullptr;      /* forward pass */
    Bin* m_bins_rev = nullptr;  /* reverse pass */
    int m_pass = 0;             /* 0 = forward, 1 = reverse */
    bool m_dual_pass = true;
    int m_valid_bins = 0;
    int m_paired_bins = 0;
    float m_lag_rms_deg = 0.0f;
    float m_residual_rms_deg = 0.0f;
    float m_residual_pp_deg = 0.0f;
    float m_harmonics[MAX_HARMONIC + 1] = {};
};

EncoderLinearityCalibrator& encoderLinearityCalibrator();

} // namespace Inverter
