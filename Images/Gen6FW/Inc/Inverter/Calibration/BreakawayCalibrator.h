#pragma once

#include "Inverter/Calibration/Common/CalibrationHardware.h"
#include "Inverter/Calibration/Common/EncoderTracker.h"

#include <cstdint>

namespace Inverter {

/**
 * @brief Simple standalone breakaway finder with reverse-direction sanity check.
 *
 * Ramps the stator field voltage slowly (one step per second) until the encoder
 * shows the rotor has moved.  After breakaway is found in the forward direction,
 * the field reverses and the calibrator verifies the encoder moves the other
 * way.  This rejects phantom motion and gives an early check of encoder sign.
 */
class BreakawayCalibrator {
public:
    enum class State {
        IDLE,
        HW_INIT,
        RAMP_FWD,
        VERIFY_REVERSE,
        DONE,
        FAIL
    };

    BreakawayCalibrator() = default;

    /**
     * @brief Start a breakaway search.
     *
     * @param max_voltage    Maximum peak phase voltage to apply [V].
     * @param step_voltage   Voltage increment per ramp step [V].
     * @param step_dwell_ms  Minimum time to hold each step before advancing [ms].
     * @param detect_cycles  Encoder mechanical cycles that count as movement.
     * @param reverse_cycles Encoder mechanical cycles required during reverse check.
     * @return true if the gate-driver power sequence was started.
     */
    bool start(float max_voltage = 10.0f,
               float step_voltage = 0.5f,
               uint32_t step_dwell_ms = 1000U,
               float detect_cycles = 0.15f,
               float reverse_cycles = 0.10f);

    /** Non-blocking state-machine update; call at ~100 Hz. */
    void update();

    /** Abort and turn off switching. */
    void stop();

    bool isActive() const {
        return m_state != State::IDLE && m_state != State::DONE && m_state != State::FAIL;
    }
    bool isDone() const { return m_state == State::DONE; }
    bool isFailed() const { return m_state == State::FAIL; }

    State state() const { return m_state; }

    float breakawayMod() const { return m_breakaway_mod; }
    float breakawayVoltage() const { return m_breakaway_voltage; }

    static BreakawayCalibrator& instance();

private:
    void enterState(State state);
    void fail(const char* reason);
    void updateRampFwd();
    void updateVerifyReverse();
    void advanceVoltageStep();

    static float voltageToMod(float v_v, float vdc_v);
    static float modToVoltage(float m, float vdc_v);

    CalibrationHardware m_hw;
    EncoderTracker m_tracker;

    State m_state = State::IDLE;
    uint32_t m_state_start_ms = 0;
    uint32_t m_step_enter_ms = 0;
    uint32_t m_last_move_ms = 0;

    float m_max_voltage = 10.0f;
    float m_step_voltage = 0.5f;
    uint32_t m_step_dwell_ms = 1000U;
    float m_detect_cycles = 0.15f;
    float m_reverse_cycles = 0.10f;

    float m_mod = 0.0f;
    float m_breakaway_mod = 0.0f;
    float m_breakaway_voltage = 0.0f;

    float m_start_cycles = 0.0f;
    bool m_have_start = false;
    bool m_in_trusted_region = false;
    float m_max_phantom_cycles = 0.0f;

    /* Reverse-check state. */
    float m_reverse_start_cycles = 0.0f;
    bool m_have_reverse_start = false;
    float m_forward_final_cycles = 0.0f;
};

BreakawayCalibrator& breakawayCalibrator();

} // namespace Inverter
