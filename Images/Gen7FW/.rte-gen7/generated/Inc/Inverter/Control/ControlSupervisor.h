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

    State m_state = State::Idle;
    bool m_stop_requested = false;
    uint32_t m_started_ms = 0;
};

} // namespace Inverter
