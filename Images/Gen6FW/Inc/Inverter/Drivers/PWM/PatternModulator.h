#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Direct switching-state (n-pulse / SHE) modulation engine.
 *
 * Drives TIM1 channels 1-3 in output-compare set-active/set-inactive mode
 * with edges scheduled at exact pattern angles (SHE tables), instead of the
 * duty-cycle PWM path.  Complementary outputs, hardware dead-time, and the
 * break input all stay in hardware, so the fault chain (software break,
 * DESAT) is unaffected.
 *
 * Operating model:
 *  - A graph node (or shell) pushes the latest command each control step via
 *    setCommand(): modulation index, pattern phase offset, rotor electrical
 *    angle and speed, and pulse number.
 *  - enable() switches TIM1 to edge-aligned counting at the same carrier
 *    period and starts chained compare scheduling; disable() restores the
 *    center-aligned PWM duty path (50% duties).
 *  - Edge scheduling is one-edge-ahead per phase: on each compare match the
 *    ISR applies the level for the edge that just fired (explicit
 *    set-active / set-inactive, never toggle, so level can never desync)
 *    and writes the compare for the next pattern edge.  Level is always
 *    derived from the pattern function, not from a counter.
 *
 * Safety notes:
 *  - platform_pwm_set() becomes a no-op while pattern mode is enabled (the
 *    duty path must not touch CCRs while this driver owns them).
 *  - The hardware break (TIM1 EGR BG) works in any channel mode; nothing
 *    here re-enables outputs after a fault.
 */
class PatternModulator {
public:
    static PatternModulator& instance();

    /** @brief Safe to call at boot; the driver starts disabled. */
    void init();

    /**
     * @brief Update the modulation command (called every control step).
     *
     * @param m            Modulation index (0..1.15), fundamental vs square wave.
     * @param delta_rad    Pattern phase offset from the rotor angle [rad].
     * @param theta_e_rad  Rotor electrical angle [rad, 0..2pi).
     * @param omega_e_rad_s Electrical angular velocity [rad/s] (signed).
     * @param n_pulses     SHE pulse number per quarter wave (v1: 5 only).
     */
    void setCommand(float m, float delta_rad, float theta_e_rad,
                    float omega_e_rad_s, int n_pulses);

    /**
     * @brief Switch TIM1 into pattern mode and start edge scheduling.
     *
     * Must be called with the control loop running (TIM1 counting).  The
     * switch forces outputs off for a few microseconds.
     * @return true on success (false if already enabled or TIM1 unavailable).
     */
    bool enable();

    /**
     * @brief Restore the center-aligned PWM duty path at 50% duties.
     *
     * Safe to call any time; no-op when already disabled.  Also called from
     * the supervisor stop path so a fault never leaves the timer in pattern
     * configuration.
     */
    void disable();

    bool isEnabled() const { return m_enabled; }

    /** @brief TIM1 update (overflow) hook; call after the control step. */
    void onUpdateIsr();

    /**
     * @brief TIM1 capture/compare match hook for one phase (0=A, 1=B, 2=C).
     *
     * Applies the level of the edge that just fired and schedules the next.
     */
    void onMatchIsr(uint8_t phase);

private:
    PatternModulator() = default;

    /* Interpolated SHE switching angles for the current command m [rad]. */
    void sheAngles(float* out, int n) const;

    /* Wave level for a phase at pattern angle phi (+1 high, -1 low). */
    float waveLevel(float phi, float phase_offset) const;

    /* Find the next edge angle after phi (circular) for one phase. */
    float nextEdgeAngle(float phi, float phase_offset) const;

    /* Schedule the next edge for one phase, starting from current CNT. */
    void scheduleNext(uint8_t phase);

    /* Re-evaluate the next edge for every phase (update ISR). */
    void resync();

    /* ---- command state (written from thread/ISR by setCommand) ---- */
    volatile float m_cmd_m = 0.0f;
    volatile float m_cmd_delta = 0.0f;
    volatile float m_cmd_theta = 0.0f;
    volatile float m_cmd_omega = 0.0f;
    volatile int   m_cmd_n = 5;

    /* ---- runtime state ---- */
    bool     m_enabled = false;
    bool     m_moe_arm = false;     /**< set MOE on next update event. */
    uint32_t m_saved_arr = 0;       /**< center-aligned ARR to restore. */
    float    m_last_theta = 0.0f;
    float    m_level[3] = {0.0f, 0.0f, 0.0f};   /**< currently applied level. */

    static constexpr float TWO_PI = 6.28318530718f;
    static constexpr float MIN_OMEGA = 20.0f;   /**< rad/s; below this, hold. */
};

/** @brief Global instance. */
PatternModulator& patternModulator();

} // namespace Inverter
