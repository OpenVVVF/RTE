#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/AppState.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Telemetry.h"

#include "../../../generated/domain_tim_isr_generated.h"

#include <cstring>
#include <strings.h>

using Inverter::ControlSupervisor;
using Inverter::FaultManager;

/**
 * @brief Single `control <subcommand> [args...]` dispatcher for the generated
 *        control loop.
 *
 * Supported forms:
 *   control start
 *   control stop
 *   control status
 *   control set <param> <value>
 *   control get <param>
 */
class ControlCommand : public CommandInterface {
public:
    ControlCommand()
      : CommandInterface("control", "Generated control: start/stop/status/set/get",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"name", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"value", "", -1e6f, 1e6f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;

        if (strcasecmp(sub, "start") == 0) {
            if (ControlSupervisor::instance().start()) {
                Telemetry::printf("[SHELL] control started");
            } else {
                Telemetry::printf("[SHELL] control start failed");
            }
        } else if (strcasecmp(sub, "stop") == 0) {
            ControlSupervisor::instance().stop();
            Telemetry::printf("[SHELL] control stopped");
        } else if (strcasecmp(sub, "status") == 0) {
            printStatus();
        } else if (strcasecmp(sub, "set") == 0) {
            setParam(args[1].s_val, args[2].present ? args[2].f_val : 0.0f);
        } else if (strcasecmp(sub, "get") == 0) {
            getParam(args[1].s_val);
        } else {
            Telemetry::printf("[SHELL] Unknown control subcommand '%s'. Use: start/stop/status/set/get", sub);
        }
    }

private:
    void printStatus() const {
        ControlSupervisor& sup = ControlSupervisor::instance();
        Telemetry::printf("[SHELL] state=%s running=%s faulted=%s",
                          sup.stateName(),
                          sup.isRunning() ? "Y" : "N",
                          sup.isFaulted() ? "Y" : "N");
        if (FaultManager::instance().isActive()) {
            Telemetry::printf("[SHELL] active faults:");
            FaultManager::instance().printSummary();
        }
    }

    void setParam(const char* name, float value) const {
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            if (strcasecmp(name, app::g_tim_isr_params[i].name) == 0) {
                app::g_tim_isr_params[i].set(&appState.tim_isr, value);
                Telemetry::printf("[SHELL] %s = %.4f", name, static_cast<double>(value));
                return;
            }
        }
        Telemetry::printf("[SHELL] unknown parameter '%s'", name);
    }

    void getParam(const char* name) const {
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            if (strcasecmp(name, app::g_tim_isr_params[i].name) == 0) {
                const float v = app::g_tim_isr_params[i].get(&appState.tim_isr);
                Telemetry::printf("[SHELL] %s = %.4f", name, static_cast<double>(v));
                return;
            }
        }
        Telemetry::printf("[SHELL] unknown parameter '%s'", name);
    }
};

static ControlCommand sControlCmd;

void registerControlCommands(CommandManager& mgr) {
    mgr.registerCommand(&sControlCmd);
}
