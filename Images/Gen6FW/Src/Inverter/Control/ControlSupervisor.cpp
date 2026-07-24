#include "Inverter/Control/ControlSupervisor.h"

#include "Inverter/AppState.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include "../../../generated/domain_tim_isr_generated.h"

namespace Inverter {

ControlSupervisor& ControlSupervisor::instance() {
    static ControlSupervisor inst;
    return inst;
}

bool ControlSupervisor::init() {
    app::TimIsrInit(appState.tim_isr);
    m_state = State::Idle;
    Telemetry::printf("[SUP] initialized");
    return true;
}

bool ControlSupervisor::gateDriverStartup() {
    /* Assert reset before powering to ensure a clean start. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(10);

    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port,
                      GATE_DRIVER_POWER_ENABLE_Pin, GPIO_PIN_SET);
    HAL_Delay(50);

    /* Release reset and wait for the driver to signal ready. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_SET);
    HAL_Delay(50);

    if (GateDriver_IsFault()) {
        Telemetry::printf("[SUP] ERROR: gate driver fault latched");
        return false;
    }
    if (!GateDriver_IsReady()) {
        Telemetry::printf("[SUP] ERROR: gate driver not ready");
        return false;
    }
    return true;
}

bool ControlSupervisor::start() {
    if (m_state == State::Running) {
        return true;
    }
    if (m_state == State::Fault) {
        Telemetry::printf("[SUP] ERROR: faulted; clear faults first");
        return false;
    }
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[SUP] ERROR: active Critical/High faults");
        FaultManager::instance().printSummary();
        return false;
    }

    m_state = State::Starting;

    if (!gateDriverStartup()) {
        m_state = State::Fault;
        return false;
    }

    PWM_ClearFault();
    PWM_EnableFocMode();
    PWM_Start();

    if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U) {
        Telemetry::printf("[SUP] ERROR: TIM1 MOE not active after PWM start");
        GateDriver_DisableOutputs();
        m_state = State::Fault;
        return false;
    }

    /* Reset generated control state before the first ISR fires. */
    app::TimIsrStart(appState.tim_isr);

    PWM_StartUpdateInterrupt();
    m_state = State::Running;
    m_started_ms = HAL_GetTick();
    Telemetry::printf("[SUP] STARTED f_sw=%.0f Hz f_u=%.0f Hz",
                      static_cast<double>(PWM_GetFrequency()),
                      static_cast<double>(PWM_GetUpdateFrequency()));
    return true;
}

void ControlSupervisor::stop() {
    if (m_state != State::Running && m_state != State::Starting) {
        return;
    }

    m_state = State::Stopping;

    /* Zero generated outputs before stopping the ISR. */
    app::TimIsrStop(appState.tim_isr);

    PWM_Stop();
    GateDriver_DisableOutputs();
    m_state = State::Idle;
    Telemetry::printf("[SUP] STOPPED");
}

void ControlSupervisor::requestStopFromIsr() {
    m_stop_requested = true;
}

void ControlSupervisor::enterFaultState() {
    if (m_state == State::Running || m_state == State::Starting) {
        app::TimIsrStop(appState.tim_isr);
        PWM_Stop();
        GateDriver_DisableOutputs();
    }
    m_state = State::Fault;
}

void ControlSupervisor::service() {
    if (m_stop_requested) {
        m_stop_requested = false;
        stop();
        return;
    }

    /* Critical faults force an immediate transition to Fault. */
    if (m_state == State::Running || m_state == State::Starting) {
        if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical)) {
            Telemetry::printf("[SUP] critical fault -> FAULT");
            enterFaultState();
        }
    }
}

const char* ControlSupervisor::stateName() const {
    switch (m_state) {
        case State::Idle: return "IDLE";
        case State::Starting: return "STARTING";
        case State::Running: return "RUNNING";
        case State::Stopping: return "STOPPING";
        case State::Fault: return "FAULT";
    }
    return "UNKNOWN";
}

} // namespace Inverter
