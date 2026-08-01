#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/ModulationSwitch.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

#include "main.h"

using Inverter::FaultManager;
using Inverter::FaultSeverity;
using Inverter::focControlManager;
using Inverter::openLoopController;

namespace {

bool otherControlActive() {
    if (focControlManager().isRunning() || openLoopController().isRunning()) {
        Telemetry::printf("[SHE] ERROR: FOC or open-loop is running; stop it first");
        return true;
    }
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[SHE] ERROR: active Critical/High faults");
        FaultManager::instance().printSummary();
        return true;
    }
    return false;
}

/* Blocking safe-start: park at zero vector, cycle gate-driver reset, verify
 * ready, start TIM1 outputs, hand the modulation slot to the SHEPWM engine.
 * The caller stages the pattern (SHE table or N-pulse) before calling. */
bool sheSafeStart() {
    if (otherControlActive()) return false;
    if (Inverter::shepwmIsRunning()) {
        Telemetry::printf("[SHE] already running; use sheset/npset or shestop");
        return false;
    }

    PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
    GateDriver_DisableOutputs();
    HAL_Delay(10);
    GateDriver_EnableOutputs();
    HAL_Delay(10);

    if (!GateDriver_IsReady() || GateDriver_IsFault()) {
        Telemetry::printf("[SHE] ERROR: gate driver not ready or fault latched");
        GateDriver_DisableOutputs();
        return false;
    }

    PWM_ClearFault();
    PWM_Start();

    if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U) {
        Telemetry::printf("[SHE] ERROR: TIM1 MOE not active after PWM start");
        GateDriver_DisableOutputs();
        return false;
    }

    if (!Inverter::shepwmModulator().enter(0.0f, 0.0f)) {
        Telemetry::printf("[SHE] ERROR: enter failed");
        PWM_Stop();
        GateDriver_DisableOutputs();
        return false;
    }
    Inverter::setActiveModulator(&Inverter::shepwmModulator());
    return true;
}

class SheStartCommand : public CommandInterface {
public:
    SheStartCommand()
      : CommandInterface("shestart", "Start pattern output: shestart <freq> <duty_or_mi> [pulses_per_qtr]",
            {ArgSpec{"freq_hz", "Hz", 0.1f, 1000.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"duty_or_mi", "", 0.0f, 1.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"pulses", "/qtr", 0.0f, 128.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (args[2].present) {
            /* N-pulse mode: arg1 = duty (high fraction per cell). */
            const uint32_t npq = static_cast<uint32_t>(args[2].f_val + 0.5f);
            Inverter::shepwmSetPulsePattern(args[0].f_val, npq, args[1].f_val);
            if (!sheSafeStart()) return;
            Telemetry::printf("[SHE] STARTED N-pulse fe=%.2f Hz pulses/qtr=%lu duty=%.3f",
                              static_cast<double>(args[0].f_val),
                              static_cast<unsigned long>(npq),
                              static_cast<double>(args[1].f_val));
        } else {
            /* SHE table mode: arg1 = modulation index. */
            Inverter::shepwmSetPattern(args[0].f_val, args[1].f_val);
            if (!sheSafeStart()) return;
            Telemetry::printf("[SHE] STARTED fe=%.2f Hz mi=%.3f",
                              static_cast<double>(args[0].f_val),
                              static_cast<double>(args[1].f_val));
        }
    }
};

class SheStopCommand : public CommandInterface {
public:
    SheStopCommand() : CommandInterface("shestop", "Stop SHEPWM/N-pulse output") {}

    void execute(const ArgValue*, CommandContext&) override {
        if (!Inverter::shepwmIsRunning()) {
            Telemetry::printf("[SHE] not running");
            return;
        }
        Inverter::shepwmModulator().exit();
        if (Inverter::activeModulator() == &Inverter::shepwmModulator()) {
            Inverter::setActiveModulator(nullptr);
        }
        PWM_Stop();
        GateDriver_DisableOutputs();
        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        Telemetry::printf("[SHE] STOPPED");
    }
};

class SheSetCommand : public CommandInterface {
public:
    SheSetCommand()
      : CommandInterface("sheset", "Set pattern: sheset <freq> <duty_or_mi> [pulses_per_qtr] (swaps at wrap)",
            {ArgSpec{"freq_hz", "Hz", 0.1f, 1000.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"duty_or_mi", "", 0.0f, 1.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"pulses", "/qtr", 0.0f, 128.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (!Inverter::shepwmIsRunning()) {
            Telemetry::printf("[SHE] not running; use shestart");
            return;
        }
        if (args[2].present) {
            const uint32_t npq = static_cast<uint32_t>(args[2].f_val + 0.5f);
            Inverter::shepwmSetPulsePattern(args[0].f_val, npq, args[1].f_val);
            Telemetry::printf("[SHE] pattern staged: N-pulse fe=%.2f Hz pulses/qtr=%lu duty=%.3f (swaps at wrap)",
                              static_cast<double>(args[0].f_val),
                              static_cast<unsigned long>(npq),
                              static_cast<double>(args[1].f_val));
        } else {
            Inverter::shepwmSetPattern(args[0].f_val, args[1].f_val);
            Telemetry::printf("[SHE] pattern staged: fe=%.2f Hz mi=%.3f (swaps at wrap)",
                              static_cast<double>(args[0].f_val),
                              static_cast<double>(args[1].f_val));
        }
    }
};

class SheStatusCommand : public CommandInterface {
public:
    SheStatusCommand() : CommandInterface("shestatus", "Show SHEPWM/N-pulse status") {}

    void execute(const ArgValue*, CommandContext&) override {
        const uint32_t npq = Inverter::shepwmPulseCount();
        if (npq > 0) {
            Telemetry::printf("[SHE] run=%s mode=N-pulse fe=%.2f Hz pulses/qtr=%lu duty=%.3f wraps=%lu edges=%lu",
                              Inverter::shepwmIsRunning() ? "Y" : "N",
                              static_cast<double>(Inverter::shepwmFrequencyHz()),
                              static_cast<unsigned long>(npq),
                              static_cast<double>(Inverter::shepwmDuty()),
                              static_cast<unsigned long>(Inverter::shepwmWrapCount()),
                              static_cast<unsigned long>(Inverter::shepwmEdgeCount()));
        } else {
            Telemetry::printf("[SHE] run=%s mode=SHE-table fe=%.2f Hz mi=%.3f wraps=%lu edges=%lu",
                              Inverter::shepwmIsRunning() ? "Y" : "N",
                              static_cast<double>(Inverter::shepwmFrequencyHz()),
                              static_cast<double>(Inverter::shepwmModulationIndex()),
                              static_cast<unsigned long>(Inverter::shepwmWrapCount()),
                              static_cast<unsigned long>(Inverter::shepwmEdgeCount()));
        }
    }
};

/**
 * @brief Live modulator handoff at speed (step 4): open-loop ramp <-> pattern.
 *
 *   handoff 1 <pulses_per_qtr> <duty>   ramp (SPWM) -> N-pulse pattern
 *   handoff 0                           pattern -> ramp (same fe/MI)
 *
 * Thin shell wrapper over Inverter::modulationToPattern/ToRamp (which the
 * graph-facing platform_api also calls).  Phase-locked both directions.
 */
class HandoffCommand : public CommandInterface {
public:
    HandoffCommand()
      : CommandInterface("handoff", "Live handoff: 'handoff 1 <pulses> <duty>' to pattern, 'handoff 0' back to ramp",
            {ArgSpec{"to_pattern", "0/1", 0.0f, 1.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"pulses", "/qtr", 0.0f, 128.0f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"duty", "", 0.0f, 1.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (args[0].f_val >= 0.5f) {
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[HO] usage: handoff 1 <pulses_per_qtr> <duty>");
                return;
            }
            const uint32_t npq = static_cast<uint32_t>(args[1].f_val + 0.5f);
            if (!Inverter::modulationToPattern(npq, args[2].f_val)) {
                Telemetry::printf("[HO] failed: need open-loop ramp running (not FOC)");
            }
        } else {
            if (!Inverter::modulationToRamp()) {
                Telemetry::printf("[HO] failed");
            }
        }
    }
};

SheStartCommand  sSheStartCmd;
SheStopCommand   sSheStopCmd;
SheSetCommand    sSheSetCmd;
SheStatusCommand sSheStatusCmd;
/* DTCMRAM is full; command objects are main-loop-only, so this one lives in
 * AXI SRAM (runtime-constructed like all statics; NOLOAD is fine). */
HandoffCommand   sHandoffCmd __attribute__((section(".dma_buffers")));

} // namespace

#include "Inverter/Command/CommandManager.h"

void registerSheCommands(CommandManager& mgr) {
    mgr.registerCommand(&sSheStartCmd);
    mgr.registerCommand(&sSheStopCmd);
    mgr.registerCommand(&sSheSetCmd);
    mgr.registerCommand(&sSheStatusCmd);
    mgr.registerCommand(&sHandoffCmd);
}
