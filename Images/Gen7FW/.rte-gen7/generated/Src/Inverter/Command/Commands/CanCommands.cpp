#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/Drivers/CAN/CanBus.h"
#include "Inverter/Drivers/CAN/CanSession.h"
#include "Inverter/Telemetry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

/**
 * @brief `can <subcommand>` CAN bus diagnostics.
 *
 *   can status              Both buses: enable/rate/counters/PSR.
 *   can send <bus> <id> [b0..b7]   Queue a classic frame (decimal id/data).
 *   can rxdump              Last 8 received frames per bus.
 */
class CanCommand : public CommandInterface {
public:
    CanCommand()
      : CommandInterface("can", "CAN bus: status/send/rxdump",
            {ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"a1", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a2", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a3", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a4", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a5", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a6", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a7", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a8", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"a9", "", -2e9f, 2e9f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;

        if (strcasecmp(sub, "status") == 0) {
            Inverter::canBus().printStatus(0);
            Inverter::canBus().printStatus(1);
            Inverter::canSession().printStatus();
            return;
        }
        if (strcasecmp(sub, "rxdump") == 0) {
            Inverter::canBus().printRecentRx(0);
            Inverter::canBus().printRecentRx(1);
            return;
        }
        if (strcasecmp(sub, "send") == 0) {
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[SHELL] usage: can send <bus 1|2> <id> [b0..b7]");
                return;
            }
            if ((args[1].f_val != 1.0f && args[1].f_val != 2.0f) ||
                !(args[2].f_val >= 0.0f && args[2].f_val <= 2047.0f)) {
                Telemetry::printf("[SHELL] bus must be 1|2 and standard id 0..2047");
                return;
            }
            const uint8_t bus = static_cast<uint8_t>(args[1].f_val) - 1;
            const uint32_t id = static_cast<uint32_t>(args[2].f_val);
            if (args[2].f_val != static_cast<float>(id)) {
                Telemetry::printf("[SHELL] CAN id must be an integer");
                return;
            }
            uint8_t data[8] = {};
            uint8_t dlc = 0;
            for (uint8_t i = 0; i < 8 && args[3 + i].present; ++i) {
                const float byte = args[3 + i].f_val;
                if (!(byte >= 0.0f && byte <= 255.0f) ||
                    byte != static_cast<float>(static_cast<uint8_t>(byte))) {
                    Telemetry::printf("[SHELL] data bytes must be integers 0..255");
                    return;
                }
                data[i] = static_cast<uint8_t>(byte);
                dlc = i + 1;
            }
            if (Inverter::canBus().send(bus, id, false, data, dlc)) {
                Telemetry::printf("[SHELL] queued id=0x%03lX dlc=%u on bus %u",
                                  static_cast<unsigned long>(id), dlc,
                                  static_cast<unsigned>(bus + 1));
            } else {
                Telemetry::printf("[SHELL] send failed (bus disabled or bad dlc)");
            }
            return;
        }
        Telemetry::printf("[SHELL] Unknown can subcommand '%s'. Use: status/send/rxdump", sub);
    }
};

static CanCommand sCanCmd;

void registerCanCommands(CommandManager& mgr) {
    mgr.registerCommand(&sCanCmd);
}
