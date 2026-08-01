#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/Modulator.h"
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

class SheStartCommand : public CommandInterface {
public:
    SheStartCommand()
      : CommandInterface("shestart", "Start SHEPWM output (TIM5 pattern, TIM1 forced modes)",
            {ArgSpec{"freq_hz", "Hz", 0.1f, 1000.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"mod_idx", "", 0.0f, 1.0f, 0.0f, true, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (otherControlActive()) return;
        if (Inverter::shepwmIsRunning()) {
            Telemetry::printf("[SHE] already running; use sheset or shestop");
            return;
        }

        const float fe_hz = args[0].f_val;
        const float mi = args[1].f_val;

        /* Safe start (same blocking sequence as vectorscan): park at zero
         * vector, cycle gate-driver reset, verify ready. */
        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        GateDriver_DisableOutputs();
        HAL_Delay(10);
        GateDriver_EnableOutputs();
        HAL_Delay(10);

        if (!GateDriver_IsReady() || GateDriver_IsFault()) {
            Telemetry::printf("[SHE] ERROR: gate driver not ready or fault latched");
            GateDriver_DisableOutputs();
            return;
        }

        PWM_ClearFault();
        PWM_Start();

        if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U) {
            Telemetry::printf("[SHE] ERROR: TIM1 MOE not active after PWM start");
            GateDriver_DisableOutputs();
            return;
        }

        Inverter::shepwmSetPattern(fe_hz, mi);
        if (!Inverter::shepwmModulator().enter(0.0f, mi)) {
            Telemetry::printf("[SHE] ERROR: enter failed");
            PWM_Stop();
            GateDriver_DisableOutputs();
            return;
        }
        Inverter::setActiveModulator(&Inverter::shepwmModulator());

        Telemetry::printf("[SHE] STARTED fe=%.2f Hz mi=%.3f",
                          static_cast<double>(fe_hz), static_cast<double>(mi));
    }
};

class SheStopCommand : public CommandInterface {
public:
    SheStopCommand() : CommandInterface("shestop", "Stop SHEPWM output") {}

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
      : CommandInterface("sheset", "Set SHEPWM frequency/MI (applies at next cycle wrap)",
            {ArgSpec{"freq_hz", "Hz", 0.1f, 1000.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"mod_idx", "", 0.0f, 1.0f, 0.0f, true, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (!Inverter::shepwmIsRunning()) {
            Telemetry::printf("[SHE] not running; use shestart");
            return;
        }
        Inverter::shepwmSetPattern(args[0].f_val, args[1].f_val);
        Telemetry::printf("[SHE] pattern staged: fe=%.2f Hz mi=%.3f (swaps at wrap)",
                          static_cast<double>(args[0].f_val),
                          static_cast<double>(args[1].f_val));
    }
};

class SheStatusCommand : public CommandInterface {
public:
    SheStatusCommand() : CommandInterface("shestatus", "Show SHEPWM status") {}

    void execute(const ArgValue*, CommandContext&) override {
        Telemetry::printf("[SHE] run=%s fe=%.2f Hz mi=%.3f wraps=%lu edges=%lu",
                          Inverter::shepwmIsRunning() ? "Y" : "N",
                          static_cast<double>(Inverter::shepwmFrequencyHz()),
                          static_cast<double>(Inverter::shepwmModulationIndex()),
                          static_cast<unsigned long>(Inverter::shepwmWrapCount()),
                          static_cast<unsigned long>(Inverter::shepwmEdgeCount()));
    }
};

SheStartCommand  sSheStartCmd;
SheStopCommand   sSheStopCmd;
SheSetCommand    sSheSetCmd;
SheStatusCommand sSheStatusCmd;

} // namespace

#include "Inverter/Command/CommandManager.h"

void registerSheCommands(CommandManager& mgr) {
    mgr.registerCommand(&sSheStartCmd);
    mgr.registerCommand(&sSheStopCmd);
    mgr.registerCommand(&sSheSetCmd);
    mgr.registerCommand(&sSheStatusCmd);
}
