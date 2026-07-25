#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/AppState.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "../../../generated/domain_tim_isr_generated.h"
#include "../../../generated/domain_app_loop_generated.h"
#include "../../../generated/domain_adc_isr_generated.h"

#include <cstring>
#include <initializer_list>
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

/**
 * @brief `config <subcommand> [args...]` for FRAM-backed config nodes.
 *
 * Config nodes keep their live value in generated state (`cached`); FRAM is
 * only written on an explicit `config save`.
 *
 * Supported forms:
 *   config set <key> <value>   Update the live value (RAM only, immediate).
 *   config get <key>           Print the live value.
 *   config save [key]          Persist one (or all) live values to FRAM.
 *   config list                List all key/value pairs stored in FRAM.
 */
class ConfigCommand : public CommandInterface {
public:
    ConfigCommand()
      : CommandInterface("config", "FRAM config: set/get/save/list",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"key", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"value", "", -1e6f, 1e6f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;

        if (strcasecmp(sub, "set") == 0) {
            setLive(args[1].s_val, args[2].present ? args[2].f_val : 0.0f);
        } else if (strcasecmp(sub, "get") == 0) {
            getLive(args[1].s_val);
        } else if (strcasecmp(sub, "save") == 0) {
            save(args[1].present ? args[1].s_val : nullptr);
        } else if (strcasecmp(sub, "list") == 0) {
            listStored();
        } else {
            Telemetry::printf("[SHELL] Unknown config subcommand '%s'. Use: set/get/save/list", sub);
        }
    }

private:
    struct ConfigDomain {
        const RteParamDesc* descs;
        size_t count;
        void* state;
    };

    /* All generated config registries, one per timing domain. */
    static std::initializer_list<ConfigDomain> domains() {
        return {
            {app::g_app_loop_configs, app::g_app_loop_config_count, &appState.app_loop},
            {app::g_tim_isr_configs, app::g_tim_isr_config_count, &appState.tim_isr},
            {app::g_adc_isr_configs, app::g_adc_isr_config_count, &appState.adc_isr},
        };
    }

    /* Find a config entry by key. Returns nullptr when unknown. */
    static const RteParamDesc* find(const char* key, void** stateOut) {
        for (const auto& domain : domains()) {
            for (size_t i = 0; i < domain.count; ++i) {
                if (strcasecmp(key, domain.descs[i].name) == 0) {
                    *stateOut = domain.state;
                    return &domain.descs[i];
                }
            }
        }
        return nullptr;
    }

    void setLive(const char* key, float value) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(key, &state);
        if (desc == nullptr) {
            Telemetry::printf("[SHELL] unknown config key '%s'", key);
            return;
        }
        desc->set(state, value);
        Telemetry::printf("[SHELL] %s = %.4f (live; 'config save' to persist)", key,
                          static_cast<double>(value));
    }

    void getLive(const char* key) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(key, &state);
        if (desc == nullptr) {
            Telemetry::printf("[SHELL] unknown config key '%s'", key);
            return;
        }
        Telemetry::printf("[SHELL] %s = %.4f", key,
                          static_cast<double>(desc->get(state)));
    }

    void save(const char* key) const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] config store not ready");
            return;
        }
        size_t saved = 0;
        for (const auto& domain : domains()) {
            for (size_t i = 0; i < domain.count; ++i) {
                const RteParamDesc& desc = domain.descs[i];
                if (key != nullptr && strcasecmp(key, desc.name) != 0) continue;
                Inverter::RteParamStore::set(desc.name, desc.get(domain.state));
                ++saved;
            }
        }
        if (saved == 0) {
            Telemetry::printf("[SHELL] unknown config key '%s'", key != nullptr ? key : "");
            return;
        }
        if (Inverter::RteParamStore::flush()) {
            Telemetry::printf("[SHELL] %zu config value(s) saved to FRAM", saved);
        } else {
            Telemetry::printf("[SHELL] FRAM flush failed");
        }
    }

    void listStored() const {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] config store not ready");
            return;
        }
        Telemetry::printf("[SHELL] stored config keys (%zu):", Inverter::RteParamStore::count());
        Inverter::RteParamStore::iterate(printConfig, nullptr);
    }

    static bool printConfig(const char* key, float value, void*) {
        Telemetry::printf("[SHELL]   %s = %.4f", key, static_cast<double>(value));
        return true;
    }
};

/**
 * @brief `live <subcommand> [args...]` for live runtime variables.
 *
 * Maps friendly names to generated node parameters for immediate updates.
 *   live set iq_ref 5.0
 *   live set id_ref 0.0
 *   live get iq_ref
 */
class LiveCommand : public CommandInterface {
public:
    LiveCommand()
      : CommandInterface("live", "Live variables: set/get iq_ref/id_ref",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"name", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"value", "", -1e6f, 1e6f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;
        const char* name = args[1].present ? args[1].s_val : "";
        const float value = args[2].present ? args[2].f_val : 0.0f;

        const char* param = mapName(name);
        if (param == nullptr) {
            Telemetry::printf("[SHELL] unknown live variable '%s'. Use: iq_ref, id_ref", name);
            return;
        }

        if (strcasecmp(sub, "set") == 0) {
            setLive(param, value);
        } else if (strcasecmp(sub, "get") == 0) {
            getLive(param);
        } else {
            Telemetry::printf("[SHELL] Unknown live subcommand '%s'. Use: set/get", sub);
        }
    }

private:
    static const char* mapName(const char* name) {
        if (strcasecmp(name, "iq_ref") == 0) return "IqRef.amps";
        if (strcasecmp(name, "id_ref") == 0) return "IdRef.amps";
        if (strcasecmp(name, "offset_rad") == 0) return "ElecAngle.offset_rad";
        if (strcasecmp(name, "encoder_sign") == 0) return "ElecAngle.encoder_sign";
        return nullptr;
    }

    void setLive(const char* param, float value) const {
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            if (strcasecmp(param, app::g_tim_isr_params[i].name) == 0) {
                app::g_tim_isr_params[i].set(&appState.tim_isr, value);
                Telemetry::printf("[SHELL] %s = %.4f", param, static_cast<double>(value));
                return;
            }
        }
        Telemetry::printf("[SHELL] parameter '%s' not found", param);
    }

    void getLive(const char* param) const {
        for (size_t i = 0; i < app::g_tim_isr_param_count; ++i) {
            if (strcasecmp(param, app::g_tim_isr_params[i].name) == 0) {
                const float v = app::g_tim_isr_params[i].get(&appState.tim_isr);
                Telemetry::printf("[SHELL] %s = %.4f", param, static_cast<double>(v));
                return;
            }
        }
        Telemetry::printf("[SHELL] parameter '%s' not found", param);
    }
};

static ControlCommand sControlCmd;
static ConfigCommand sConfigCmd;
static LiveCommand sLiveCmd;

void registerControlCommands(CommandManager& mgr) {
    mgr.registerCommand(&sControlCmd);
    mgr.registerCommand(&sConfigCmd);
    mgr.registerCommand(&sLiveCmd);
}
