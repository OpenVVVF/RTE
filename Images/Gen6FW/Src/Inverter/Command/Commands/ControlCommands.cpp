#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/AppState.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Drivers/Sensors/TemperatureSensors.h"
#include "Inverter/Telemetry.h"

#include "Inverter/platform_api.h"

#include <cmath>

#include "../../../generated/domain_tim_isr_generated.h"
#include "../../../generated/domain_app_loop_generated.h"
#include "../../../generated/domain_adc_isr_generated.h"

#include <cstring>
#include <strings.h>

using Inverter::ControlSupervisor;
using Inverter::FaultManager;

/**
 * @brief `control <subcommand>` for the generated control loop.
 *
 * Supported forms:
 *   control start
 *   control stop
 *   control status
 */
class ControlCommand : public CommandInterface {
public:
    ControlCommand()
      : CommandInterface("control", "Generated control: start/stop/status",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING}}) {}

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
        } else {
            Telemetry::printf("[SHELL] Unknown control subcommand '%s'. Use: start/stop/status", sub);
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
};

/**
 * @brief `config <subcommand> [args...]` for FRAM-backed config nodes.
 *
 * Config nodes keep their live value in generated state (`cached`); FRAM is
 * only written on an explicit save.
 *
 * Supported forms:
 *   config set <key> <value>   Live test: update RAM value (immediate, volatile).
 *   config get <key>           Print the live RAM value.
 *   config save <key>          Persist one live value to FRAM.
 *   config saveall             Persist all config values to FRAM.
 *   config delete <key>        Remove a saved FRAM entry (live value unchanged
 *                              until reboot).
 *   config deleteall           Wipe all saved RTE config entries from FRAM.
 *   config list                List all key/value pairs stored in FRAM.
 */
class ConfigCommand : public CommandInterface {
public:
    ConfigCommand()
      : CommandInterface("config", "FRAM config: set/get/save/saveall/delete/deleteall/list",
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
            if (!args[1].present) {
                Telemetry::printf("[SHELL] usage: config save <key> (or config saveall)");
                return;
            }
            saveOne(args[1].s_val);
        } else if (strcasecmp(sub, "saveall") == 0) {
            saveAll();
        } else if (strcasecmp(sub, "delete") == 0) {
            if (!args[1].present) {
                Telemetry::printf("[SHELL] usage: config delete <key>");
                return;
            }
            deleteOne(args[1].s_val);
        } else if (strcasecmp(sub, "deleteall") == 0) {
            deleteAll();
        } else if (strcasecmp(sub, "list") == 0) {
            listStored();
        } else {
            Telemetry::printf("[SHELL] Unknown config subcommand '%s'. Use: set/get/save/saveall/delete/deleteall/list", sub);
        }
    }

private:
    struct ConfigDomain {
        const RteParamDesc* descs;
        size_t count;
        void* state;
    };

    /* All generated config registries, one per timing domain. NOTE: do not
     * return std::initializer_list here — its backing array dangles after
     * return (caused wild reads / hangs on target). */
    static const ConfigDomain* allDomains(size_t& outCount) {
        static const ConfigDomain kDomains[] = {
            {app::g_app_loop_configs, app::g_app_loop_config_count, &appState.app_loop},
            {app::g_tim_isr_configs, app::g_tim_isr_config_count, &appState.tim_isr},
            {app::g_adc_isr_configs, app::g_adc_isr_config_count, &appState.adc_isr},
        };
        outCount = sizeof(kDomains) / sizeof(kDomains[0]);
        return kDomains;
    }

    /* Find a config entry by key. Returns nullptr when unknown. */
    static const RteParamDesc* find(const char* key, void** stateOut) {
        size_t domainCount = 0;
        const ConfigDomain* domains = allDomains(domainCount);
        for (size_t d = 0; d < domainCount; ++d) {
            const ConfigDomain& domain = domains[d];
            for (size_t i = 0; i < domain.count; ++i) {
                if (strcasecmp(key, domain.descs[i].name) == 0) {
                    *stateOut = domain.state;
                    return &domain.descs[i];
                }
            }
        }
        return nullptr;
    }

    static bool storeReady() {
        if (!Inverter::RteParamStore::isReady()) {
            Telemetry::printf("[SHELL] config store not ready");
            return false;
        }
        return true;
    }

    static void reportFlush(const char* what, size_t n) {
        if (Inverter::RteParamStore::flush()) {
            Telemetry::printf("[SHELL] %s (%d)", what, static_cast<int>(n));
        } else {
            Telemetry::printf("[SHELL] FRAM flush failed");
        }
    }

    void setLive(const char* key, float value) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(key, &state);
        if (desc == nullptr) {
            /* Fall through to the raw KV store so any base-image key
             * (e.g. Hw.Temp.*) can be tuned live without a graph config node. */
            if (!storeReady()) return;
            platform_config_set(key, value);
            Telemetry::printf("[SHELL] %s = %.4f (raw KV key, live; 'config save %s' to persist)",
                              key, static_cast<double>(value), key);
            return;
        }
        desc->set(state, value);
        Telemetry::printf("[SHELL] %s = %.4f (live; 'config save %s' to persist)", key,
                          static_cast<double>(value), key);
    }

    void getLive(const char* key) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(key, &state);
        if (desc == nullptr) {
            /* Fall through to the raw KV store (see setLive). */
            if (!storeReady()) return;
            float value = 0.0f;
            if (!Inverter::RteParamStore::get(key, &value)) {
                Telemetry::printf("[SHELL] raw KV key '%s' not found", key);
                return;
            }
            Telemetry::printf("[SHELL] %s = %.4f (raw KV key)", key,
                              static_cast<double>(value));
            return;
        }
        Telemetry::printf("[SHELL] %s = %.4f", key,
                          static_cast<double>(desc->get(state)));
    }

    void saveOne(const char* key) const {
        if (!storeReady()) return;
        void* state = nullptr;
        const RteParamDesc* desc = find(key, &state);
        if (desc == nullptr) {
            /* Raw KV key (e.g. Hw.Temp.*): the cache was already updated by
             * `config set`; just flush it. */
            float value = 0.0f;
            if (!Inverter::RteParamStore::get(key, &value)) {
                Telemetry::printf("[SHELL] unknown config key '%s'", key);
                return;
            }
            reportFlush("config value saved to FRAM", 1);
            return;
        }
        Inverter::RteParamStore::set(desc->name, desc->get(state));
        reportFlush("config value saved to FRAM", 1);
    }

    void saveAll() const {
        if (!storeReady()) return;
        size_t saved = 0;
        size_t domainCount = 0;
        const ConfigDomain* domains = allDomains(domainCount);
        for (size_t d = 0; d < domainCount; ++d) {
            const ConfigDomain& domain = domains[d];
            for (size_t i = 0; i < domain.count; ++i) {
                const RteParamDesc& desc = domain.descs[i];
                Inverter::RteParamStore::set(desc.name, desc.get(domain.state));
                ++saved;
            }
        }
        reportFlush("config values saved to FRAM", saved);
    }

    void deleteOne(const char* key) const {
        if (!storeReady()) return;
        if (!Inverter::RteParamStore::remove(key)) {
            Telemetry::printf("[SHELL] key '%s' not found in FRAM", key);
            return;
        }
        Inverter::RteParamStore::flush();
        Telemetry::printf("[SHELL] deleted '%s' from FRAM (live value unchanged until reboot)", key);
    }

    void deleteAll() const {
        if (!storeReady()) return;
        const size_t n = Inverter::RteParamStore::count();
        if (Inverter::RteParamStore::clear()) {
            reportFlush("all RTE config entries deleted from FRAM", n);
        } else {
            Telemetry::printf("[SHELL] clear failed");
        }
    }

    void listStored() const {
        if (!storeReady()) return;
        Telemetry::printf("[SHELL] stored config keys (%d):",
                          static_cast<int>(Inverter::RteParamStore::count()));
        Inverter::RteParamStore::iterate(printConfig, nullptr);
    }

    static bool printConfig(const char* key, float value, void*) {
        Telemetry::printf("[SHELL]   %s = %.4f", key, static_cast<double>(value));
        return true;
    }
};

/**
 * @brief `var <subcommand> [args...]` for graph-owned variables.
 *
 * RAM-only, machine-owned state (not persisted to FRAM).
 *   var set <name> <value>   Set a var's Stored value.
 *   var get <name>           Read it back.
 *   var list                 List all vars and their values.
 */
class VarCommand : public CommandInterface {
public:
    VarCommand()
      : CommandInterface("var", "Graph variables: set/get/list",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"name", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"value", "", -1e6f, 1e6f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;
        if (strcasecmp(sub, "set") == 0) {
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[SHELL] usage: var set <name> <value>");
                return;
            }
            setVar(args[1].s_val, args[2].f_val);
        } else if (strcasecmp(sub, "get") == 0) {
            getVar(args[1].s_val);
        } else if (strcasecmp(sub, "list") == 0) {
            listVars();
        } else {
            Telemetry::printf("[SHELL] Unknown var subcommand '%s'. Use: set/get/list", sub);
        }
    }

private:
    struct VarDomain {
        const RteParamDesc* descs;
        size_t count;
        void* state;
    };

    /* NOTE: no initializer_list return-by-value here (dangling backing
     * array = wild iteration = main-loop hang).  Plain static array. */
    static const VarDomain* allDomains(size_t& outCount) {
        static const VarDomain kDomains[] = {
            {app::g_app_loop_vars, app::g_app_loop_var_count, &appState.app_loop},
            {app::g_tim_isr_vars, app::g_tim_isr_var_count, &appState.tim_isr},
            {app::g_adc_isr_vars, app::g_adc_isr_var_count, &appState.adc_isr},
        };
        outCount = sizeof(kDomains) / sizeof(kDomains[0]);
        return kDomains;
    }

    static const RteParamDesc* find(const char* name, void** stateOut) {
        size_t domainCount = 0;
        const VarDomain* domains = allDomains(domainCount);
        for (size_t d = 0; d < domainCount; ++d) {
            const VarDomain& domain = domains[d];
            for (size_t i = 0; i < domain.count; ++i) {
                if (strcasecmp(name, domain.descs[i].name) == 0) {
                    *stateOut = domain.state;
                    return &domain.descs[i];
                }
            }
        }
        return nullptr;
    }

    void setVar(const char* name, float value) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(name, &state);
        if (desc == nullptr) {
            Telemetry::printf("[SHELL] unknown var '%s'", name);
            return;
        }
        desc->set(state, value);
        Telemetry::printf("[SHELL] %s = %.4f", name, static_cast<double>(value));
    }

    void getVar(const char* name) const {
        void* state = nullptr;
        const RteParamDesc* desc = find(name, &state);
        if (desc == nullptr) {
            Telemetry::printf("[SHELL] unknown var '%s'", name);
            return;
        }
        Telemetry::printf("[SHELL] %s = %.4f", name,
                          static_cast<double>(desc->get(state)));
    }

    void listVars() const {
        size_t domainCount = 0;
        const VarDomain* domains = allDomains(domainCount);
        for (size_t d = 0; d < domainCount; ++d) {
            const VarDomain& domain = domains[d];
            for (size_t i = 0; i < domain.count; ++i) {
                Telemetry::printf("[SHELL]   %s = %.4f", domain.descs[i].name,
                                  static_cast<double>(domain.descs[i].get(domain.state)));
            }
        }
    }
};

/**
 * @brief `temp` — live view of the four temperature channels.
 *
 * Prints enable state, conversion type, pin voltage, computed divider
 * resistance and temperature so the right sensor type can be identified
 * (flip Hw.Temp.<ch>.Type with `config set` and compare against ambient).
 */
class TempCommand : public CommandInterface {
public:
    TempCommand()
      : CommandInterface("temp", "Temperature channels: V / ohm / degC live view",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (args[0].present && strcasecmp(args[0].s_val, "types") == 0) {
            Telemetry::printf("[SHELL] sensor types: 0=disabled 1=NTC-beta 2=PTC-beta "
                              "3=KTY84-130/150 4=linear-RTD 5=PT1000 6=PT100 7=KTY83-110");
            return;
        }
        static const char* NAMES[Inverter::TemperatureSensors::NUM_CHANNELS] = {
            "inv1", "inv2", "inv3", "motor",
        };
        for (uint8_t ch = 0; ch < Inverter::TemperatureSensors::NUM_CHANNELS; ++ch) {
            bool enabled = false, oor = false;
            uint8_t type = 0;
            float volts = NAN, ohms = NAN, tempC = NAN;
            Inverter::temperatureSensors().channelStatus(ch, enabled, type, volts,
                                                         ohms, tempC, oor);
            Telemetry::printf("[SHELL] %s: en=%d type=%s V=%.3f R=%.0f ohm T=%.1f C%s",
                              NAMES[ch], enabled ? 1 : 0,
                              Inverter::TemperatureSensors::typeName(type),
                              static_cast<double>(volts), static_cast<double>(ohms),
                              static_cast<double>(tempC), oor ? " OUT-OF-RANGE" : "");
        }
        Telemetry::printf("[SHELL] throttle: A=%.3f V B=%.3f V",
                          static_cast<double>(platform_get_throttle_a()),
                          static_cast<double>(platform_get_throttle_b()));
        Inverter::temperatureSensors().debugStatus();
    }
};

static ControlCommand sControlCmd;
static ConfigCommand sConfigCmd;
static VarCommand sVarCmd;
static TempCommand sTempCmd;

void registerControlCommands(CommandManager& mgr) {
    mgr.registerCommand(&sControlCmd);
    mgr.registerCommand(&sConfigCmd);
    mgr.registerCommand(&sVarCmd);
    mgr.registerCommand(&sTempCmd);
}
