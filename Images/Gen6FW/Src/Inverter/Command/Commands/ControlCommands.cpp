#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/AppState.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
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
        } else if (strcasecmp(sub, "save") == 0) {
            saveParams();
        } else if (strcasecmp(sub, "load") == 0) {
            loadParams();
        } else if (strcasecmp(sub, "list") == 0) {
            listParams();
        } else if (strcasecmp(sub, "clear") == 0) {
            clearParams();
        } else {
            Telemetry::printf("[SHELL] Unknown control subcommand '%s'. Use: start/stop/status/set/get/save/load/list/clear", sub);
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
                /* Auto-persist the single key so it survives reboot. */
                if (Inverter::RteParamStore::isReady()) {
                    Inverter::RteParamStore::set(name, value);
                    Inverter::RteParamStore::flush();
                }
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

    void saveParams() const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] param store not ready");
            return;
        }
        /* Write every generated parameter into the KV store. */
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            const float v = app::g_tim_isr_params[i].get(&appState.tim_isr);
            Inverter::RteParamStore::set(app::g_tim_isr_params[i].name, v);
        }
        if (Inverter::RteParamStore::flush()) {
            Telemetry::printf("[SHELL] %zu params saved to FRAM", app::g_tim_isr_param_count);
        } else {
            Telemetry::printf("[SHELL] FRAM flush failed");
        }
    }

    void loadParams() const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] param store not ready");
            return;
        }
        size_t loaded = 0;
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            float v;
            if (Inverter::RteParamStore::get(app::g_tim_isr_params[i].name, &v)) {
                app::g_tim_isr_params[i].set(&appState.tim_isr, v);
                ++loaded;
            }
        }
        Telemetry::printf("[SHELL] %zu params loaded from FRAM", loaded);
    }

    static bool printParam(const char* key, float value, void*) {
        Telemetry::printf("[SHELL]   %s = %.4f", key, static_cast<double>(value));
        return true;
    }

    void listParams() const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] param store not ready");
            return;
        }
        Telemetry::printf("[SHELL] stored params (%zu):", Inverter::RteParamStore::count());
        Inverter::RteParamStore::iterate(printParam, nullptr);
    }

    void clearParams() const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] param store not ready");
            return;
        }
        if (Inverter::RteParamStore::clear() && Inverter::RteParamStore::flush()) {
            Telemetry::printf("[SHELL] param store cleared");
        } else {
            Telemetry::printf("[SHELL] clear failed");
        }
    }
};

static ControlCommand sControlCmd;

void registerControlCommands(CommandManager& mgr) {
    mgr.registerCommand(&sControlCmd);
}
