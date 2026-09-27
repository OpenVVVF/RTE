#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/CoprocessorFaults.h"
#include "Inverter/SafetyLink.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "tim.h"

#include <strings.h>

using Inverter::ControlSupervisor;
using Inverter::FaultManager;
using Inverter::FaultSeverity;
using Inverter::FaultSource;

namespace {

using Inverter::FaultBits;

constexpr FaultBits GATE_FAULTS =
    FaultBits::bit(FaultSource::GateDriver) |
    FaultBits::bit(FaultSource::PwmBreak) |
    FaultBits::bit(FaultSource::GateDriverUvlo);

const FaultBits MAX22530_FAULTS =
    FaultBits::bit(FaultSource::Max22530Ov) |
    FaultBits::bit(FaultSource::Max22530Uv) |
    FaultBits::bit(FaultSource::Max22530Adc) |
    FaultBits::bit(FaultSource::Max22530Comm) |
    FaultBits::bit(FaultSource::Max22530Field);

constexpr FaultBits SUPPLY_FAULTS =
    FaultBits::bit(FaultSource::SupplyPvd) |
    FaultBits::bit(FaultSource::SupplyAvd) |
    FaultBits::bit(FaultSource::SupplyVosrdy);

const char* severityName(FaultSeverity severity) {
    switch (severity) {
        case FaultSeverity::Warning: return "warning";
        case FaultSeverity::High: return "high";
        case FaultSeverity::Critical: return "critical";
    }
    return "unknown";
}

FaultBits allFaultMask() {
    FaultBits mask;
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        mask.set(FaultManager::metaTable()[i].source);
    }
    return mask;
}

FaultBits severityMask(FaultSeverity severity) {
    FaultBits mask;
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        const Inverter::FaultMeta& meta = FaultManager::metaTable()[i];
        if (meta.severity == severity) {
            mask.set(meta.source);
        }
    }
    return mask;
}

bool scopeMask(const char* scope, FaultBits& mask) {
    if (scope == nullptr || scope[0] == '\0' || strcasecmp(scope, "all") == 0) {
        mask = allFaultMask();
        return true;
    }
    if (strcasecmp(scope, "warning") == 0 || strcasecmp(scope, "warnings") == 0) {
        mask = severityMask(FaultSeverity::Warning);
        return true;
    }
    if (strcasecmp(scope, "main") == 0) {
        mask = allFaultMask();
        return true;
    }
    if (strcasecmp(scope, "high") == 0) {
        mask = severityMask(FaultSeverity::High);
        return true;
    }
    if (strcasecmp(scope, "critical") == 0) {
        mask = severityMask(FaultSeverity::Critical);
        return true;
    }

    const FaultSource source = FaultManager::sourceFromName(scope);
    if (source != FaultSource::None) {
        mask = FaultBits::bit(source);
        return true;
    }
    return false;
}

bool powerStageActive() {
    /* TIM1 MOE is NOT an actuation indicator on Gen7: it stays latched for
     * measurement (PWM_ClearFault arms it, PWM_Stop leaves it set) while the
     * TIM1/ADC ISR runs permanently.  Only the software state of the control
     * paths says whether the motor is being driven. */
    const ControlSupervisor::State generatedState = ControlSupervisor::instance().state();
    return generatedState == ControlSupervisor::State::Starting ||
           generatedState == ControlSupervisor::State::Running ||
           generatedState == ControlSupervisor::State::Stopping ||
           Inverter::focControlManager().isRunning() ||
           Inverter::openLoopController().isRunning();
}

struct GateResetStatus {
    bool checked = false;
    bool ready = false;
    bool fault = false;
};

void printSources() {
    Telemetry::printf("[FAULT] clear scopes: all, main, coprocessor, warning, high, critical, or one main source:");
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        const Inverter::FaultMeta& meta = FaultManager::metaTable()[i];
        Telemetry::printf("[FAULT][MAIN][%s][%s] %s - %s",
                          severityName(meta.severity), meta.category,
                          meta.name, meta.description);
    }
    Telemetry::printf("[FAULT][COPROCESSOR] GateDriver, PowerStuckOn, PowerFeedbackLost, MainHeartbeatLost, Internal");
}

bool resetGateHardware(const FaultBits& mask, GateResetStatus& status) {
    if (!mask.intersects(GATE_FAULTS)) {
        return true;
    }
    if (powerStageActive()) {
        Telemetry::printf("[FAULT] clear refused: stop control/PWM before clearing gate faults");
        return false;
    }

    const bool outputsWereReleased =
        HAL_GPIO_ReadPin(GATE_DRIVER_RESET_GPIO_Port, GATE_DRIVER_RESET_Pin) == GPIO_PIN_SET;

    GateDriver_EnablePower(true);
    HAL_Delay(50);
    GateDriver_ResetPulse();
    HAL_Delay(100);
    PWM_ClearBreakFlag();

    status.checked = true;
    status.ready = GateDriver_IsReady();
    status.fault = GateDriver_IsFault();

    /* Re-arm the one-shot TIM1 break interrupt: the driver's latched /FLT
     * has just been reset-pulsed, so the break input is inactive again and
     * the next genuine DESAT event must be able to fire. */
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_BREAK);
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_BREAK);

    if (!outputsWereReleased) {
        GateDriver_DisableOutputs();
    }

    Telemetry::printf("[FAULT] gate reset: ready=%s fault=%s MOE=%lu outputs_restored=%s",
                      status.ready ? "Y" : "N", status.fault ? "Y" : "N",
                      static_cast<unsigned long>((TIM1->BDTR & TIM_BDTR_MOE) != 0U),
                      outputsWereReleased ? "released" : "reset");
    return true;
}

void resetMax22530Hardware(const FaultBits& mask) {
    if (!mask.intersects(MAX22530_FAULTS)) {
        return;
    }

    Inverter::MAX22530& adc = Inverter::dcLinkVoltageSensor().adc();
    const bool interruptCleared = adc.clearInterruptStatus();
    const bool filterCleared = adc.clearFilter(3);
    __HAL_GPIO_EXTI_CLEAR_IT(VSENSE_ISO_ADC_INTERRUPT_Pin);
    HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
    Telemetry::printf("[FAULT] MAX22530 reset: interrupt=%s filter=%s",
                      interruptCleared ? "cleared" : "FAILED",
                      filterCleared ? "cleared" : "FAILED");
}

void recheckGateHardware(const GateResetStatus& status) {
    if (!status.checked) {
        return;
    }
    if (status.fault) {
        FaultManager::instance().raise(FaultSource::GateDriver,
                                       Inverter::FaultReason::GateDriverNotReady);
    }
    if (!status.ready) {
        FaultManager::instance().raise(FaultSource::GateDriverUvlo,
                                       Inverter::FaultReason::GateDriverNotReady);
    }
}

void recheckSupplyHardware(const FaultBits& mask) {
    if (!mask.intersects(SUPPLY_FAULTS)) {
        return;
    }
    if (mask.test(FaultSource::SupplyVosrdy) &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == 0U) {
        FaultManager::instance().raise(FaultSource::SupplyVosrdy,
                                       Inverter::FaultReason::VosNotReady);
    }
    if (mask.test(FaultSource::SupplyPvd) &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) != 0U) {
        FaultManager::instance().raise(FaultSource::SupplyPvd,
                                       Inverter::FaultReason::PvdTriggered);
    }
    if (mask.test(FaultSource::SupplyAvd) &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_AVDO) != 0U) {
        FaultManager::instance().raise(FaultSource::SupplyAvd,
                                       Inverter::FaultReason::AvdTriggered);
    }
}

void clearFaults(const char* scope) {
    const bool coprocessor_only = scope != nullptr &&
                                  strcasecmp(scope, "coprocessor") == 0;
    const bool main_only = scope != nullptr && strcasecmp(scope, "main") == 0;
    FaultBits mask;
    if (!coprocessor_only && !scopeMask(scope, mask)) {
        Telemetry::printf("[FAULT] unknown clear scope '%s'; run 'fault sources'", scope);
        return;
    }

    const bool clear_coprocessor = coprocessor_only ||
        (!main_only && (scope == nullptr || strcasecmp(scope, "all") == 0 ||
                        strcasecmp(scope, "critical") == 0 ||
                        mask.intersects(GATE_FAULTS)));
    if (clear_coprocessor && powerStageActive()) {
        Telemetry::printf("[FAULT] clear refused: stop control before clearing coprocessor faults");
        return;
    }
    auto& coprocessor = Inverter::CoprocessorFaults::instance();
    if (clear_coprocessor && !coprocessor.fresh()) {
        Telemetry::printf("[FAULT][COPROCESSOR] clear refused: status unavailable or stale");
        return;
    }

    if (coprocessor_only) {
        if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical)) {
            Telemetry::printf("[FAULT][COPROCESSOR] clear refused: main Critical fault remains");
            return;
        }
        SafetyLink_Resume();
        HAL_Delay(60); /* Two fresh PD8 edges before the G474 clear check. */
        if (coprocessor.requestClear()) {
            Telemetry::printf("[FAULT][COPROCESSOR] clear requested; check 'fault status' for result");
        }
        return;
    }

    if (mask.intersects(GATE_FAULTS) && powerStageActive()) {
        Telemetry::printf("[FAULT] clear refused: stop control/PWM before clearing gate faults");
        return;
    }
    FaultManager& faults = FaultManager::instance();
    faults.clearMask(mask);

    resetMax22530Hardware(mask);

    bool coprocessor_requested = false;
    if (clear_coprocessor) {
        if (faults.isSeverityActive(FaultSeverity::Critical)) {
            Telemetry::printf("[FAULT][COPROCESSOR] clear deferred: main Critical fault remains");
        } else {
            SafetyLink_Resume();
            HAL_Delay(60); /* Allow the G474 to see two fresh heartbeat edges. */
            if (coprocessor.requestClear()) {
                coprocessor_requested = true;
                HAL_Delay(120); /* G474 processes the high level in its loop. */
                coprocessor.service(); /* Release PD9 before another request. */
                Telemetry::printf("[FAULT][COPROCESSOR] clear requested; check 'fault status' for result");
            }
        }
    }

    GateResetStatus gateStatus;
    if (!resetGateHardware(mask, gateStatus)) {
        return;
    }
    recheckGateHardware(gateStatus);
    recheckSupplyHardware(mask);
    const FaultBits blockingMask = severityMask(FaultSeverity::Critical) |
                                   severityMask(FaultSeverity::High);
    const bool resetSupervisor = mask.intersects(blockingMask);
    const bool supervisorReset = !resetSupervisor ||
                                 ControlSupervisor::instance().resetFaultState();
    faults.publishStatus();

    const char* scopeName = (scope == nullptr || scope[0] == '\0') ? "all" : scope;
    Telemetry::printf("[FAULT] main clear '%s' complete; coprocessor=%s active=0x%08lX supervisor=%s",
                      scopeName,
                      coprocessor_requested ? "pending" :
                      (clear_coprocessor ? "not cleared" : "unchanged"),
                      static_cast<unsigned long>(faults.activeFlags()),
                      supervisorReset ? ControlSupervisor::instance().stateName()
                                      : "FAULT (blocking fault remains)");
    if (faults.isActive()) {
        Telemetry::printf("[FAULT] a live or unselected condition remains active:");
    } else {
        Telemetry::printf("[FAULT] no faults active; persistent conditions may reassert");
    }
    faults.printSummary();
}

} // namespace

class FaultCommand : public CommandInterface {
public:
    FaultCommand()
      : CommandInterface("fault", "Fault status/sources/clear/test; reset aliases clear",
            {ArgSpec{"action", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"scope", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* action = args[0].s_val;
        const char* scope = args[1].present ? args[1].s_val : nullptr;

        if (strcasecmp(action, "status") == 0 || strcasecmp(action, "list") == 0) {
            if (scope != nullptr) {
                Telemetry::printf("[FAULT] usage: fault status");
                return;
            }
            FaultManager::instance().publishStatus();
            FaultManager::instance().printSummary();
            Inverter::CoprocessorFaults::instance().printStatus();
            return;
        }
        if (strcasecmp(action, "sources") == 0) {
            if (scope != nullptr) {
                Telemetry::printf("[FAULT] usage: fault sources");
                return;
            }
            printSources();
            return;
        }
        if (strcasecmp(action, "clear") == 0 || strcasecmp(action, "reset") == 0) {
            clearFaults(scope);
            return;
        }
        if (strcasecmp(action, "test") == 0) {
            test(scope);
            return;
        }

        Telemetry::printf("[FAULT] unknown action '%s'; use status, sources, clear, or test", action);
    }

private:
    void test(const char* sourceName) const {
        if (sourceName == nullptr || sourceName[0] == '\0') {
            Telemetry::printf("[FAULT] usage: fault test <source>; run 'fault sources'");
            return;
        }
        const FaultSource source = FaultManager::sourceFromName(sourceName);
        if (source == FaultSource::None) {
            Telemetry::printf("[FAULT] unknown source '%s'; run 'fault sources'", sourceName);
            return;
        }
        FaultManager::instance().testFault(source);
        FaultManager::instance().publishStatus();
        Telemetry::printf("[FAULT] injected test fault %s", sourceName);
    }
};

class ClearFaultAliasCommand : public CommandInterface {
public:
    ClearFaultAliasCommand()
      : CommandInterface("clearfault", "Legacy alias for: fault clear [scope]",
            ArgSpec{"scope", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        clearFaults(args[0].present ? args[0].s_val : nullptr);
    }
};

class ClearCommand : public CommandInterface {
public:
    ClearCommand()
      : CommandInterface("clear", "Alias for: fault clear [scope]",
            {ArgSpec{"target", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"scope", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (strcasecmp(args[0].s_val, "fault") != 0 &&
            strcasecmp(args[0].s_val, "faults") != 0) {
            Telemetry::printf("[FAULT] usage: clear fault [scope]");
            return;
        }
        clearFaults(args[1].present ? args[1].s_val : nullptr);
    }
};

static FaultCommand sFaultCmd;
static ClearFaultAliasCommand sClearFaultAliasCmd;
static ClearCommand sClearCmd;

#include "Inverter/Command/CommandManager.h"

void registerFaultCommands(CommandManager& mgr) {
    mgr.registerCommand(&sFaultCmd);
    mgr.registerCommand(&sClearFaultAliasCmd);
    mgr.registerCommand(&sClearCmd);
}
