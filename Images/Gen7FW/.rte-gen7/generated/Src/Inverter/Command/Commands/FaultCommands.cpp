#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include <strings.h>

using Inverter::ControlSupervisor;
using Inverter::FaultManager;
using Inverter::FaultSeverity;
using Inverter::FaultSource;

namespace {

constexpr uint32_t bit(FaultSource source) {
    return static_cast<uint32_t>(source);
}

constexpr uint32_t GATE_FAULTS =
    bit(FaultSource::GateDriver) |
    bit(FaultSource::PwmBreak) |
    bit(FaultSource::GateDriverUvlo);

constexpr uint32_t MAX22530_FAULTS =
    bit(FaultSource::Max22530Ov) |
    bit(FaultSource::Max22530Uv) |
    bit(FaultSource::Max22530Adc) |
    bit(FaultSource::Max22530Comm) |
    bit(FaultSource::Max22530Field);

constexpr uint32_t SUPPLY_FAULTS =
    bit(FaultSource::SupplyPvd) |
    bit(FaultSource::SupplyAvd) |
    bit(FaultSource::SupplyVosrdy);

const char* severityName(FaultSeverity severity) {
    switch (severity) {
        case FaultSeverity::Warning: return "warning";
        case FaultSeverity::High: return "high";
        case FaultSeverity::Critical: return "critical";
    }
    return "unknown";
}

uint32_t allFaultMask() {
    uint32_t mask = 0U;
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        mask |= bit(FaultManager::metaTable()[i].source);
    }
    return mask;
}

uint32_t severityMask(FaultSeverity severity) {
    uint32_t mask = 0U;
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        const Inverter::FaultMeta& meta = FaultManager::metaTable()[i];
        if (meta.severity == severity) {
            mask |= bit(meta.source);
        }
    }
    return mask;
}

bool scopeMask(const char* scope, uint32_t& mask) {
    if (scope == nullptr || scope[0] == '\0' || strcasecmp(scope, "all") == 0) {
        mask = allFaultMask();
        return true;
    }
    if (strcasecmp(scope, "warning") == 0 || strcasecmp(scope, "warnings") == 0) {
        mask = severityMask(FaultSeverity::Warning);
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
        mask = bit(source);
        return true;
    }
    return false;
}

bool powerStageActive() {
    const ControlSupervisor::State generatedState = ControlSupervisor::instance().state();
    return generatedState == ControlSupervisor::State::Starting ||
           generatedState == ControlSupervisor::State::Running ||
           generatedState == ControlSupervisor::State::Stopping ||
           Inverter::focControlManager().isRunning() ||
           Inverter::openLoopController().isRunning() ||
           (TIM1->BDTR & TIM_BDTR_MOE) != 0U;
}

struct GateResetStatus {
    bool checked = false;
    bool ready = false;
    bool fault = false;
};

void printSources() {
    Telemetry::printf("[FAULT] clear scopes: all, warning, high, critical, or one source:");
    for (size_t i = 0; i < FaultManager::metaCount(); ++i) {
        const Inverter::FaultMeta& meta = FaultManager::metaTable()[i];
        Telemetry::printf("[FAULT][%s][%s] %s - %s",
                          severityName(meta.severity), meta.category,
                          meta.name, meta.description);
    }
}

bool resetGateHardware(uint32_t mask, GateResetStatus& status) {
    if ((mask & GATE_FAULTS) == 0U) {
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

    if (!outputsWereReleased) {
        GateDriver_DisableOutputs();
    }

    Telemetry::printf("[FAULT] gate reset: ready=%s fault=%s MOE=%lu outputs_restored=%s",
                      status.ready ? "Y" : "N", status.fault ? "Y" : "N",
                      static_cast<unsigned long>((TIM1->BDTR & TIM_BDTR_MOE) != 0U),
                      outputsWereReleased ? "released" : "reset");
    return true;
}

void resetMax22530Hardware(uint32_t mask) {
    if ((mask & MAX22530_FAULTS) == 0U) {
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

void recheckSupplyHardware(uint32_t mask) {
    if ((mask & SUPPLY_FAULTS) == 0U) {
        return;
    }
    if ((mask & bit(FaultSource::SupplyVosrdy)) != 0U &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == 0U) {
        FaultManager::instance().raise(FaultSource::SupplyVosrdy,
                                       Inverter::FaultReason::VosNotReady);
    }
    if ((mask & bit(FaultSource::SupplyPvd)) != 0U &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_PVDO) != 0U) {
        FaultManager::instance().raise(FaultSource::SupplyPvd,
                                       Inverter::FaultReason::PvdTriggered);
    }
    if ((mask & bit(FaultSource::SupplyAvd)) != 0U &&
        __HAL_PWR_GET_FLAG(PWR_FLAG_AVDO) != 0U) {
        FaultManager::instance().raise(FaultSource::SupplyAvd,
                                       Inverter::FaultReason::AvdTriggered);
    }
}

void clearFaults(const char* scope) {
    uint32_t mask = 0U;
    if (!scopeMask(scope, mask)) {
        Telemetry::printf("[FAULT] unknown clear scope '%s'; run 'fault sources'", scope);
        return;
    }

    GateResetStatus gateStatus;
    if (!resetGateHardware(mask, gateStatus)) {
        return;
    }
    FaultManager& faults = FaultManager::instance();
    if (mask == allFaultMask()) {
        faults.clearAll();
    } else {
        faults.clear(static_cast<FaultSource>(mask));
    }

    resetMax22530Hardware(mask);
    recheckGateHardware(gateStatus);
    recheckSupplyHardware(mask);
    const uint32_t blockingMask = severityMask(FaultSeverity::Critical) |
                                  severityMask(FaultSeverity::High);
    const bool resetSupervisor = (mask & blockingMask) != 0U;
    const bool supervisorReset = !resetSupervisor ||
                                 ControlSupervisor::instance().resetFaultState();
    faults.publishStatus();

    const char* scopeName = (scope == nullptr || scope[0] == '\0') ? "all" : scope;
    Telemetry::printf("[FAULT] clear '%s' complete; active=0x%08lX supervisor=%s",
                      scopeName,
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
