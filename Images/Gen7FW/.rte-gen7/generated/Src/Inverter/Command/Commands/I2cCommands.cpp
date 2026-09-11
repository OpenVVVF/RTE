#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Drivers/I2C/I2cSensors.h"
#include "Inverter/Telemetry.h"

#include <cmath>
#include <cstdio>
#include <strings.h>

using Inverter::I2cSensors;
using Inverter::i2cSensors;

/**
 * @brief Scan an I2C bus for ACKing addresses: `i2cscan [4|5]` (default both).
 *
 * Intended as the FIRST bring-up command on new Gen7 hardware (see
 * docs/I2C_HW_Bringup.md).  Probes 0x08..0x77 with a 2 ms timeout from shell
 * context; worst case roughly 250 ms per bus.
 */
class I2cScanCommand : public CommandInterface {
public:
    I2cScanCommand()
      : CommandInterface("i2cscan", "Scan I2C bus 4 (rail mon) / 5 (temp) for devices",
            ArgSpec{"bus", "4/5", 4.0f, 5.0f, 0.0f, false, ArgSpec::INT}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        I2cSensors& s = i2cSensors();
        uint8_t buses[2] = {4, 5};
        uint8_t nbus = 2;
        if (args[0].present) {
            const int32_t b = args[0].i_val;
            if (b != 4 && b != 5) {
                Telemetry::printf("[SHELL] usage: i2cscan [4|5]");
                return;
            }
            buses[0] = static_cast<uint8_t>(b);
            nbus = 1;
        }
        for (uint8_t i = 0; i < nbus; ++i) {
            uint8_t found[16] = {0};
            const uint32_t n = s.scan(buses[i], found, sizeof(found));
            if (n == 0) {
                Telemetry::printf("[SHELL] I2C%u: no devices found", buses[i]);
                continue;
            }
            char line[128];
            int pos = snprintf(line, sizeof(line), "[SHELL] I2C%u devices (%lu):",
                               buses[i], static_cast<unsigned long>(n));
            /* Cap pos at the buffer end: snprintf returns what *would* have
             * been written, which can exceed the buffer when many devices
             * answer. */
            if (pos >= static_cast<int>(sizeof(line))) {
                pos = static_cast<int>(sizeof(line)) - 1;
            }
            for (uint32_t d = 0; d < n && pos > 0; ++d) {
                const size_t room = sizeof(line) - static_cast<size_t>(pos);
                const int w = snprintf(&line[pos], room, " 0x%02X", found[d]);
                if (w < 0 || static_cast<size_t>(w) >= room) {
                    break;  /* buffer full: stop appending */
                }
                pos += w;
            }
            Telemetry::printf("%s", line);
        }
        Telemetry::printf("[SHELL] note: expected 0x48 on I2C5 (temp), 0x40 on I2C4 (rail mon)");
    }
};

/**
 * @brief On-board temperature sensor status: `onb_temp` / `onb_temp reload`.
 */
class OnbTempCommand : public CommandInterface {
public:
    OnbTempCommand()
      : CommandInterface("onb_temp", "Onboard I2C temp sensor status (arg 'reload' re-reads config)",
            ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        I2cSensors& s = i2cSensors();
        if (args[0].present && strcasecmp(args[0].s_val, "reload") == 0) {
            s.reloadConfig();
            return;
        }
        const auto& t = s.tempSensor();
        if (!s.tempPresent()) {
            Telemetry::printf("[SHELL] onb_temp: ABSENT @ I2C5/0x%02X (lazy re-probe every 5 s)",
                              t.address());
            return;
        }
        Telemetry::printf("[SHELL] onb_temp: %s @ I2C5/0x%02X mode=%s T=%.2f C (crit %.0f C)",
                          t.partName(), t.address(),
                          t.extendedMode() ? "13-bit" : "12-bit",
                          static_cast<double>(s.lastTempC()),
                          static_cast<double>(s.tempCritC()));
        const uint32_t err = s.tempBus().errorCount();
        const uint32_t tmo = s.tempBus().timeoutCount();
        if (err || tmo) {
            Telemetry::printf("[SHELL]   bus errors=%lu timeouts=%lu",
                              static_cast<unsigned long>(err),
                              static_cast<unsigned long>(tmo));
        }
    }
};

/**
 * @brief Rail monitor status: `rails` / `rails reload`.
 */
class RailsCommand : public CommandInterface {
public:
    RailsCommand()
      : CommandInterface("rails", "I2C rail monitor status: part, V/A/W per rail (arg 'reload' re-reads config)",
            ArgSpec{"subcommand", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        I2cSensors& s = i2cSensors();
        if (args[0].present && strcasecmp(args[0].s_val, "reload") == 0) {
            s.reloadConfig();
            return;
        }
        const auto& r = s.railMonitor();
        if (!s.railPresent()) {
            Telemetry::printf("[SHELL] rail_mon: ABSENT @ I2C4/0x%02X (lazy re-probe every 5 s); "
                              "last IDs mfg=0x%04X dev=0x%04X",
                              r.address(), r.lastMfgId(), r.lastDevId());
            return;
        }
        Telemetry::printf("[SHELL] rail_mon: %s @ I2C4/0x%02X CAL=0x%04X Ilsb=%.6f A",
                          r.partName(), r.address(), r.shuntCalValue(),
                          static_cast<double>(r.currentLsbA()));
        const uint8_t nch = r.channels();
        for (uint8_t ch = 0; ch < nch && ch < Inverter::RailMonitor::MAX_CHANNELS; ++ch) {
            const auto& smp = s.railSample(ch);
            if (std::isfinite(smp.bus_v)) {
                Telemetry::printf("[SHELL]   rail%u: %.3f V  %.3f A  %.2f W",
                                  ch + 1,
                                  static_cast<double>(smp.bus_v),
                                  static_cast<double>(smp.current_a),
                                  static_cast<double>(smp.power_w));
            } else {
                Telemetry::printf("[SHELL]   rail%u: no sample yet", ch + 1);
            }
        }
        const uint32_t err = s.railBus().errorCount();
        const uint32_t tmo = s.railBus().timeoutCount();
        if (err || tmo) {
            Telemetry::printf("[SHELL]   bus errors=%lu timeouts=%lu",
                              static_cast<unsigned long>(err),
                              static_cast<unsigned long>(tmo));
        }
    }
};

static I2cScanCommand  sI2cScanCmd;
static OnbTempCommand  sOnbTempCmd;
static RailsCommand    sRailsCmd;

#include "Inverter/Command/CommandManager.h"

void registerI2cCommands(CommandManager& mgr) {
    mgr.registerCommand(&sI2cScanCmd);
    mgr.registerCommand(&sOnbTempCmd);
    mgr.registerCommand(&sRailsCmd);
}
