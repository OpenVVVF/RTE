#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Lifecycle manager for the RTE-generated control loop.
 *
 * Owns gate-driver sequencing, PWM start/stop, and fault checks for the
 * generated tim_isr domain.  The base image keeps all safety hardware here;
 * generated code only implements the control law.
 */
class ControlSupervisor {
public:
    enum class State {
        Idle,
        Starting,
        Running,
        Stopping,
        Fault,
    };

    static ControlSupervisor& instance();

    /**
     * @brief One-time init.  Calls app::TimIsrInit() and leaves PWM stopped.
     *
     * Must be called after all hardware services (current sense, encoder,
     * DC-link) are ready.  Does NOT start PWM.
     */
    bool init();

    /**
     * @brief Safe startup sequence: release gate driver, clear faults, start PWM.
     *
     * Blocks until the gate driver is ready or a timeout/fault occurs.
     */
    bool start();

    /**
     * @brief Safe shutdown: zero outputs, stop PWM, assert gate-driver reset.
     */
    void stop();

    /**
     * @brief ISR-safe stop request.  The main loop will perform the actual stop.
     */
    void requestStopFromIsr();

    /**
     * @brief Return a faulted generated-control supervisor to Idle.
     *
     * Refuses while any Critical or High software fault remains active.
     */
    bool resetFaultState();

    bool isRunning() const { return m_state == State::Running; }
    bool isFaulted() const { return m_state == State::Fault; }
    State state() const { return m_state; }
    const char* stateName() const;

    /**
     * @brief Main-loop service: check faults and finish stop requests.
     */
    void service();

private:
    ControlSupervisor() = default;

    bool gateDriverStartup();
    void enterFaultState();

    /* SSO-pathway detector debounce windows and thresholds (see service()). */
    static constexpr uint32_t GATE_POWER_LOSS_DEBOUNCE_MS = 100;
    static constexpr uint32_t TORQUE_LOSS_DEBOUNCE_MS = 150;
    static constexpr float    TORQUE_LOSS_IQ_EMA_MAX_A = 4.0f;
    static constexpr float    TORQUE_LOSS_IQ_REF_MIN_A = 5.0f;
    static constexpr float    TORQUE_LOSS_VQ_LIMIT_FRAC = 0.9f;
    static constexpr float    TORQUE_LOSS_IQ_EMA_ALPHA = 0.2f;

    State m_state = State::Idle;
    bool m_stop_requested = false;
    uint32_t m_started_ms = 0;
    uint32_t m_gate_not_ready_since = 0;
    uint32_t m_torque_loss_since = 0;
    float    m_iq_abs_ema = 0.0f;
};

} // namespace Inverter
