#include "Inverter/Control/ControlSupervisor.h"

#include "Inverter/AppState.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"
#include "Inverter/platform_api.h"

#include "main.h"

#include "../../../generated/domain_tim_isr_generated.h"

#include <cmath>

namespace Inverter {

namespace {

void resetGeneratedState() {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app::TimIsrStart(appState.tim_isr);
    __DMB();
    __set_PRIMASK(primask);
}

void zeroGeneratedState() {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app::TimIsrStop(appState.tim_isr);
    __DMB();
    __set_PRIMASK(primask);
}

} // namespace

ControlSupervisor& ControlSupervisor::instance() {
    static ControlSupervisor inst;
    return inst;
}

bool ControlSupervisor::init() {
    platform_set_control_outputs_enabled(false);
    app::TimIsrInit(appState.tim_isr);
    PWM_StartUpdateInterrupt();
    m_state = State::Idle;
    Telemetry::log("control_state", stateName());
    Telemetry::printf("[SUP] initialized");
    return true;
}

bool ControlSupervisor::gateDriverStartup() {
    /* Normal NCD57100 startup sequencing: assert /RST (clears any latched
     * fault), make sure the power rail is on, then release /RST and let the
     * driver come ready.  Keep the /RST low pulse short: the NCx5710y
     * datasheet's 8-10 ms window on /RST invokes the DSCHK diagnostic
     * instead of a plain fault reset. */
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(1);

    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port,
                      GATE_DRIVER_POWER_ENABLE_Pin, GPIO_PIN_SET);
    HAL_Delay(50);

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

    /* The native FOC and the generated graph control both write PWM duties;
     * running both at once corrupts both loops.  foc start refuses while the
     * graph control runs (FocCommands), so refuse the reverse here too. */
    if (focControlManager().isRunning()) {
        Telemetry::printf("[SUP] ERROR: INTERNAL TEST native FOC is running; stop it first (foc stop)");
        return false;
    }

    /* Spinning-rotor guard: while coasting the graph computes no actuation
     * (the vector PI is gated by control_outputs_enabled), so the first
     * driven vector at start can only match the back-EMF if that EMF is
     * within what the bridge can produce.  Above the voltage ceiling any
     * driven state is uncontrolled rectification into the windings —
     * measured on this bench as a real >300 A phase spike when start was
     * issued at ~-2000 RPM on a 50 V bus.  Refuse: the safe state for a
     * fast-spinning rotor is six-switch-open until it coasts down. */
    {
        const float vdc_v = Inverter::dcLinkVoltageSensor().voltage();
        float lambda_wb = 0.0f;
        if (RteParamStore::isReady()) {
            (void)RteParamStore::get("Motor.Lambda", &lambda_wb);
        }
        const float pole_pairs = Inverter::MotorCalibration::instance().pole_count * 0.5f;
        const float rpm_mech = Inverter::encoderADC().rpmMech();
        constexpr float MAX_BEMF_FRACTION = 0.8f;  /* of SVPWM linear phase peak */
        float ceiling_rpm = 1000.0f;               /* conservative fallback */
        if (lambda_wb > 0.0f && vdc_v > 5.0f && pole_pairs > 0.0f) {
            /* bemf_phase_peak = lambda * |we|, we = rpm * pole_pairs * 2pi/60;
             * SVPWM linear phase peak = vdc/sqrt(3). */
            ceiling_rpm = MAX_BEMF_FRACTION * (vdc_v / 1.7320508f) /
                          (lambda_wb * pole_pairs * 0.10471976f);
        }
        if (std::fabs(rpm_mech) > ceiling_rpm) {
            Telemetry::printf("[SUP] ERROR: rotor spinning too fast to start: rpm=%.0f ceiling=%.0f mech (vdc=%.1f V lambda=%.3f Wb); coast down first",
                              static_cast<double>(rpm_mech),
                              static_cast<double>(ceiling_rpm),
                              static_cast<double>(vdc_v),
                              static_cast<double>(lambda_wb));
            return false;
        }
    }

    platform_set_control_outputs_enabled(false);
    m_state = State::Starting;

    if (!gateDriverStartup()) {
        m_state = State::Fault;
        Telemetry::log("control_state", stateName());
        return false;
    }

    /* Reset generated control state BEFORE outputs are re-enabled, so a
     * previously wound-up PI can never drive the motor. */
    platform_set_control_outputs_enabled(false);
    resetGeneratedState();

    /* The timer keeps sampling while idle, but actuator writes are gated.
     * Its CCR preload/active registers can therefore still contain the last
     * powered voltage vector.  Load a neutral vector as a safe default. */
    PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
    TIM1->EGR = TIM_EGR_UG;

    PWM_ClearFault();
    PWM_EnableFocMode();

    /* Arm graph actuation BEFORE enabling the power stage, then wait one
     * update period so the CCRs are loaded with the graph's own computed
     * vector (feed-forward back-EMF match while coasting, zero vector at
     * standstill).  The previous order (PWM_Start first) drove a blind
     * 50/50/50 zero vector for up to one update period; into a spinning
     * motor that is an uncontrolled driven state. */
    platform_set_control_outputs_enabled(true);
    HAL_Delay(1);

    PWM_Start();

    if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U) {
        /* Diagnostics: why is MOE blocked?  Report the live pin states and
         * the break-source mux so a pin-vs-comparator break is decidable
         * from the log alone. */
        Telemetry::printf("[SUP] ERROR: TIM1 MOE not active after PWM start");
        Telemetry::printf("[SUP] diag: PE15(break pin)=%d PC11(/flt)=%d PC12(/rdy)=%d",
                          (int)(GPIOE->IDR & GPIO_PIN_15) ? 1 : 0,
                          (int)(GPIOC->IDR & GPIO_PIN_11) ? 1 : 0,
                          (int)(GPIOC->IDR & GPIO_PIN_12) ? 1 : 0);
        Telemetry::printf("[SUP] diag: TIM1_AF1=0x%08lX TIM1_BDTR=0x%08lX TIM1_SR=0x%08lX",
                          (unsigned long)TIM1->AF1,
                          (unsigned long)TIM1->BDTR,
                          (unsigned long)TIM1->SR);
        platform_set_control_outputs_enabled(false);
        zeroGeneratedState();
        PWM_DisableFocMode();
        PWM_Stop();
        GateDriver_DisableOutputs();
        m_state = State::Fault;
        Telemetry::log("control_state", stateName());
        return false;
    }

    /* Lock the encoder sample stream onto the control timebase (TIM1 TRGO2
     * update events) so the FOC never sees a stall/catch-up angle step from
     * the independent TIM2 clock beating against TIM1. */
    Inverter::encoderADC().useSynchronizedTrigger(true);

    m_state = State::Running;
    Telemetry::log("control_state", stateName());
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

    /* Keep the TIM ISR alive for measurement and telemetry. Suppress graph
     * actuator writes first, then zero its state atomically. start() resets
     * all generated state again before outputs can be enabled. */
    platform_set_control_outputs_enabled(false);
    zeroGeneratedState();

    /* Back to the free-running TIM2 encoder trigger (always sampling). */
    Inverter::encoderADC().useSynchronizedTrigger(false);

    PWM_DisableFocMode();
    PWM_Stop();
    GateDriver_DisableOutputs();
    m_state = State::Idle;
    Telemetry::log("control_state", stateName());
    Telemetry::printf("[SUP] STOPPED");
}

void ControlSupervisor::requestStopFromIsr() {
    m_stop_requested = true;
}

bool ControlSupervisor::resetFaultState() {
    if (m_state != State::Fault) {
        return true;
    }
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        return false;
    }

    m_stop_requested = false;
    platform_set_control_outputs_enabled(false);
    m_state = State::Idle;
    Telemetry::log("control_state", stateName());
    Telemetry::printf("[SUP] fault state reset -> IDLE");
    return true;
}

void ControlSupervisor::enterFaultState() {
    platform_set_control_outputs_enabled(false);
    if (m_state == State::Running || m_state == State::Starting) {
        Inverter::encoderADC().useSynchronizedTrigger(false);
        zeroGeneratedState();
        PWM_DisableFocMode();
        PWM_Stop();
        GateDriver_DisableOutputs();
    }
    m_state = State::Fault;
    Telemetry::log("control_state", stateName());
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
