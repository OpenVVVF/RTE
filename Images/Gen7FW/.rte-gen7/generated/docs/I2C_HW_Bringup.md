# Gen7 I2C Hardware Bring-up

Gen7 hardware adds two I2C buses.  Per the `STM32CubeMX.ioc` (ground truth):

| Bus  | Function               | Pins                          | Kernel clock | Timing reg | Result   |
|------|------------------------|-------------------------------|--------------|------------|----------|
| I2C5 | Onboard temp sensor    | PF0 = SDA, PF1 = SCL          | 137.5 MHz    | 0x60404E72 | ~100 kHz |
| I2C4 | Voltage/rail monitor   | PD12 = SCL, PF15 = SDA        | 137.5 MHz    | 0x60404E72 | ~100 kHz |

Labels: `ONBOARD_TEMP_SENSE_I2C_SDA/SCL` (I2C5), `VOLTAGE_MONITOR_I2C_SCL/SDA` (I2C4).
7-bit addressing, single address mode, general call off, clock stretching
allowed.  Rise/fall time budget 100 ns per the `.ioc`.  `MX_I2C4_Init()` and
`MX_I2C5_Init()` live in `Src/i2c.c` (hand-maintained CubeMX-style; a CubeMX
regen must not duplicate them — see the warning banner at the top of
`Src/i2c.c` and `main.c`).

**Bring-up caveat:** PF0/PF1 carry I2C5 on **AF6** in the driver (`AF4` on these
pins is I2C2).  The `.ioc` does not record the AF selection, so confirm against
real hardware (or a CubeMX regen diff) at first power-up.

## Expected parts

The exact assembled part numbers are **not yet known**; the firmware
auto-detects against the real candidate families.

### I2C5 — on-board temperature sensor (default addr 0x48, range 0x48–0x4F)

Any of the pointer-register 12-bit family; all share register map and decode
(0.0625 °C/LSB, code in bits 15:4 of register 0x00):

| Candidate  | Distinguishing feature                              |
|------------|-----------------------------------------------------|
| TMP1075    | `DEVICE_ID` reg 0x0F = `0x7500` -> identified as TMP1075 |
| TMP102     | no ID register (0x0F reads 0x0000) -> "generic"     |
| PCT2075    | no ID register (0x0F reads 0x0000) -> "generic"     |

Extended 13-bit mode (config `EM` bit) is decoded automatically from the
device's config register.

### I2C4 — rail monitor (default addr 0x40, configurable)

Bus voltage + shunt current + power.  Auto-detect order:

| Order | Part    | ID check                                  | CAL written  | Channels |
|-------|---------|-------------------------------------------|--------------|----------|
| 1     | INA226  | reg 0xFE = `0x5449` ("TI"), 0xFF = `0x2260` | `CAL = 0.00512 / (Ilsb * Rsh)`, Ilsb = MaxA/2^15 | 1 |
| 2     | INA3221 | reg 0xFE = `0x5449`, 0xFF = `0x3220`      | none (current computed in firmware) | 3 |
| 3     | INA228  | reg 0x3E = `0x5449`, (0x3F >> 4) = `0x228` | `SHUNT_CAL = 13107.2e6 * Ilsb * Rsh` (ADCRANGE=0), Ilsb = MaxA/2^19 | 1 |

Known TI parts with an unrecognised die ID are *not* configured — the rails
stay absent and `rails` prints the raw mfg/dev IDs so support can be added.

## First power-up / wiring check order

1. `i2cscan` — scan both buses (or `i2cscan 5` / `i2cscan 4`).  Expected:
   `0x48` ACKs on I2C5, `0x40` ACKs on I2C4.
   - Nothing at all on one bus -> check the connector/continuity and that the
     expected pull-ups are populated (SDA/SCL should idle high, ~3.3 V).
   - SDA and SCL both stuck low -> swapped short / solder bridge.
   - In-regs garbage vs no-ACK distinguishes wiring from addressing issues.
2. `onb_temp` — should identify the part (TMP1075 vs generic) and report a
   plausible board temperature.  Sanity check: breathe on the sensor,
   watch `onb_temp_c` rise on the telemetry stream.
3. `rails` — should identify the monitor (INA226/INA228/INA3221) and print the
   cal value and rail readings.  Cross-check bus voltage against a DMM.
4. Only then trust the telemetry stream / warning faults.

## Shell commands

| Command              | Action                                                     |
|----------------------|------------------------------------------------------------|
| `i2cscan [4\|5]`     | ACK scan of I2C4/I2C5 (0x08–0x77), prints device list      |
| `onb_temp`           | Part, address, last temperature, bus error counters        |
| `onb_temp reload`    | Re-read KV config, re-probe (after `config set`)           |
| `rails`              | Part, CAL, per-rail V/A/W, bus error counters, last IDs    |
| `rails reload`       | Re-read KV config, re-probe (after `config set`)           |

## Config keys (FRAM via RteParamStore; `config set <key> <value>`, then
`config saveall` to persist; `onb_temp reload` / `rails reload` applies them
without reboot)

| Key                | Default | Meaning                                   |
|--------------------|---------|-------------------------------------------|
| `OnbTemp.Addr`     | 72 (0x48) | 7-bit temp sensor address               |
| `OnbTemp.CritC`    | 85      | over-temperature Warning threshold [degC] |
| `RailMon.Addr`     | 64 (0x40) | 7-bit rail monitor address              |
| `RailMon.ShuntOhm` | 0.005   | shunt resistor [ohm]                      |
| `RailMon.MaxA`     | 10      | max expected current [A] (CAL reference)  |
| `RailMon.OvV`      | 0       | rail overvoltage Warning [V]; 0 = off     |
| `RailMon.UvV`      | 0       | rail undervoltage Warning [V]; 0 = off    |

## Telemetry keys

| Key          | Source                                    |
|--------------|-------------------------------------------|
| `onb_temp_c` | onboard temperature [degC] (I2C5)         |
| `rail_v`     | rail 1 bus voltage [V]                    |
| `rail_a`     | rail 1 current [A]                        |
| `rail_w`     | rail 1 power [W] (INA3221: V*I derived)   |
| `rail2_v`, `rail2_a`, `rail3_v`, `rail3_a` | INA3221 channels 2/3 only |

## Warning faults (non-latching-trip, log-only Warning severity)

`OnboardOvertemperature` (5 degC hysteresis), `RailOvervoltage`,
`RailUndervoltage` (0.5 V hysteresis); all sustain 500 ms before raising.
The drivers are polled from the main loop only — never from an ISR — at 5 Hz
(`POLL_MS = 200`), with a 20 ms probe timeout at init so a missing device can
never delay boot; absent devices are re-probed every 5 s.  Three consecutive
poll failures mark a device absent (one log line) and re-arm the probe.

## Host-side verification

`tests/host/build_and_run.sh` compiles `OnboardTempSensor.cpp` and
`RailMonitor.cpp` for the host (plain g++ -std=c++17, no HAL) against mock
register models of TMP1075/TMP102, INA226, INA3221 and INA228, and asserts the
decode/encode math plus the detect/absent/error paths.  All I2C traffic in
the drivers funnels through `II2cBus::transfer()` / `II2cBus::isReady()`
(`Inc/Inverter/Drivers/I2C/I2cBus.h`), which is what makes this possible.

## Assumptions made (verify on real hardware)

- TMP1075 `DEVICE_ID` = `0x7500`; TMP102/PCT2075 read `0x0000` at pointer 0x0F.
- INA226 die ID `0x2260`, INA3221 die ID `0x3220`, INA228 device ID bits
  [15:4] = `0x228` (rev bits ignored).
- INA226 internal transfer: `CURRENT = SHUNT*CAL/2048`,
  `POWER = CURRENT*BUS/20000` — used by the mock, matching the datasheet.
- INA228 register fields are 20-bit, left-aligned in 24-bit registers
  (consistent with the Adafruit INA228 driver).
- PF0/PF1 = I2C5 on AF6 (see top of file).
- 100 kHz standard-mode timing from `0x60404E72` at 137.5 MHz kernel clock:
  tLOW ≈ 5.8 µs, tHIGH ≈ 4.0 µs.
