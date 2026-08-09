#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Fully automatic motor profiling routine.
 *
 * Sequentially runs the existing calibrators to determine:
 *   1. Motor pole count and encoder cycles per revolution (shared rotation).
 *   2. Encoder offset.
 *   3. Phase-to-phase resistance.
 *
 * The routine takes no motor-specific constants.  All limits are conservative
 * to keep power dissipation low:
 *   - maximum modulation index during rotation: 0.35
 *   - resistance calibration current: 10 A (20 A overcurrent abort)
 */
class AutoCalibrationCoordinator {
public:
    AutoCalibrationCoordinator() = default;

    /**
     * @brief Start the automatic profiling routine.
     *
     * The motor must be stopped and no other calibration may be active.
     */
    bool start();

    /** Calibration stages, in dependency order. */
    enum class State {
        IDLE,
        POLE,
        OFFSET,
        SETTLE,
        RESISTANCE,
        INDUCTANCE,       /**< PMSM Ld/Lq (existing InductanceCalibrator). */
        INDUCTION_PARAMS, /**< Induction-machine sigma_Ls / tau_r. */
        FLUX,
        DONE,
        FAIL
    };

    /**
     * @brief Run only a contiguous slice of the full profiling sequence.
     *
     * Stages before `first` are skipped; their prerequisites are read from the
     * RTE KV store (e.g. OFFSET needs Motor.Poles and
     * Motor.Encoder.SinCos.CyclesRev).  After `last` completes the results are
     * persisted and the routine finishes.
     *
     * @param save_results  If false, the routine skips FRAM/RTE persistence
     *                      at the end.  Useful for dry-runs or when the user
     *                      wants to inspect results before committing them.
     */
    bool startSlice(State first, State last, bool save_results = true);

    /**
     * @brief Abort a running routine and return to idle.
     */
    void stop();

    /**
     * @brief Non-blocking state-machine update.  Call at ~100 Hz from the main
     * loop.
     */
    void update();

    /** @brief True while profiling is in progress. */
    bool isActive() const {
        return m_state != State::IDLE && m_state != State::DONE &&
               m_state != State::FAIL;
    }

    /** @brief True if the last routine finished successfully. */
    bool isDone() const { return m_state == State::DONE; }

    /** @brief True if the last routine failed. */
    bool isFailed() const { return m_state == State::FAIL; }

    /** @brief Human-readable status string for telemetry/shell. */
    const char* stateName() const;

    float lastPoles() const { return m_poles; }
    float lastEncoderCyclesPerRev() const { return m_encoder_cycles_per_rev; }
    float lastBreakawayMod() const { return m_breakaway_mod; }
    float lastEncoderOffset() const { return m_encoder_offset; }
    float lastResistanceUv() const { return m_r_uv; }
    float lastResistanceUw() const { return m_r_uw; }
    float lastResistanceVw() const { return m_r_vw; }
    float lastResistanceAverage() const { return m_r_avg; }

    /**
     * @brief Override the resistance calibration target current for the next run.
     *
     * A value <= 0 disables the override and uses the default RES_MAX_CURRENT_A.
     * The override is cleared by stop()/fail()/finish().
     */
    void setResistanceTargetCurrent(float amps) { m_custom_res_current_a = amps; }

    /**
     * @brief Override inductance calibration parameters for the next run.
     *
     * Any value <= 0 disables that override.  Cleared by stop()/fail()/finish().
     */
    void setInductanceParams(float max_a, float ac_a, float freq_hz) {
        m_custom_ind_max_a = max_a;
        m_custom_ind_ac_a = ac_a;
        m_custom_ind_freq_hz = freq_hz;
    }

    /**
     * @brief Override induction calibration parameters for the next run.
     *
     * Any value <= 0 disables that override.  Cleared by stop()/fail()/finish().
     */
    void setInductionParams(float max_flux_a, float ac_voltage_pct,
                            float ac_freq_hz, float lm_estimate_h) {
        m_custom_ind_flux_a = max_flux_a;
        m_custom_ind_ac_voltage_pct = ac_voltage_pct;
        m_custom_ind_ac_freq_hz = ac_freq_hz;
        m_custom_ind_lm_h = lm_estimate_h;
    }

    static AutoCalibrationCoordinator& instance();

private:
    void enterState(State state);
    void fail(const char* reason_fmt, ...);
    void finish();
    bool startInductanceCal();
    bool startInductionMotorCal();

    State m_state = State::IDLE;
    State m_slice_last = State::FLUX;
    bool m_full_run = false;
    uint32_t m_state_enter_ms = 0;

    float m_poles = 0.0f;
    float m_encoder_cycles_per_rev = 0.0f;
    float m_pole_cal_encoder_cycles_per_rev = 0.0f;
    float m_breakaway_mod = 0.0f;
    float m_encoder_offset = 0.0f;
    float m_encoder_sign = -1.0f;
    float m_r_uv = 0.0f;
    float m_r_uw = 0.0f;
    float m_r_vw = 0.0f;
    float m_r_avg = 0.0f;
    bool m_inductance_ran = false;
    bool m_flux_ran = false;
    bool m_induction_ran = false;
    bool m_save_results = true;
    float m_custom_res_current_a = 0.0f; /**< override for next resistance cal. */
    float m_custom_ind_max_a = 0.0f;     /**< override for next inductance cal. */
    float m_custom_ind_ac_a = 0.0f;
    float m_custom_ind_freq_hz = 0.0f;
    float m_custom_ind_flux_a = 0.0f;    /**< override for next induction cal. */
    float m_custom_ind_ac_voltage_pct = 0.0f;
    float m_custom_ind_ac_freq_hz = 0.0f;
    float m_custom_ind_lm_h = 0.0f;
};

AutoCalibrationCoordinator& autoCalibrationCoordinator();

} // namespace Inverter
