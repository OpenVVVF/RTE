# Gen7 coprocessor safety kernel

This directory contains the HAL-independent gate-power policy, a heartbeat
checker, and the G474 GPIO adapter. The G474 begins with PC7 low. After checking
that PWR2 feedback is low, it accepts two edges on the H7's PD8 heartbeat and
PWR1 feedback before closing the second series power switch. An absent or
stalled heartbeat, asserted /FAULT, or lost switch feedback latches PC7 off.

## Schematic basis

Pin mapping was checked by exporting the KiCad XML netlists for
`InverterGen5/Hardware/Chassis2/Boards/ControlBoard/ControlBoard.kicad_sch`
and `InverterGen5/Hardware/Chassis2/Boards/GateDriver/GateDriver.kicad_sch`.

| G474 pin | Net | Role |
| --- | --- | --- |
| PC7 | `GATE_DRIVE_PWR2_ENABLE` | Second series gate-power switch; low inhibits. |
| PC15 | `GATE_DRIVE_PWR2_FEEDBACK` | Second switch feedback. |
| PC14 | `GATE_DRIVE_PWR1_FEEDBACK` | Main switch feedback. |
| PB10 | `FAULT` | Shared active-low gate-driver fault input. |
| PC6 | `READY` | Shared gate-driver ready input; high means ready. |
| PA10 | `DRIVER_RESET` | Shared active-low reset; kept released by this milestone. |
| PC9 | `MP_RESET` | Main MCU reset control; not used as the first fault action. |
| PB9 | `COPROCESSOR_SYNC` | H7 PD8 heartbeat input, with pull-down. |
| PC13 | `COPROCESSOR_WAKEUP` | H7 PD9 guarded fault-clear request, with pull-down. |
| PC10 / PC11 | H7 USART3 RX / TX | Fault status to H7 / H7 telemetry to USB bridge. |

The gate-driver netlist connects U5 (PWR1) output to U6 (PWR2) input. Both
switches must be on for the +12 V gate supply. Main MCU PD5 also connects to
`DRIVER_RESET`. The current main image drives that net push-pull and the
KiCad netlist shows no external pull-up. Therefore this milestone does not
assert coprocessor PA10 low; a board-level reset-net design decision is needed
before that independent shutdown path can be enabled.

## Current behavior and limits

- The coprocessor starts PC7 low, checks for impossible PWR2 feedback while
  inhibited, and latches a fault if it sees that feedback high. PWR1 and PWR2
  feedback and the shared /FAULT net are checked while powered.
- A latched fault keeps PC7 low. `fault clear coprocessor` or `fault clear all`
  sends a PD9 clear request. The G474 accepts it only if PWR2 feedback is off,
  /FAULT is released, and the H7 heartbeat is live. Clearing the latch leaves
  PWR2 off until ordinary arming conditions pass. The heartbeat changes every
  20 ms; absence of an edge
  for over 250 ms revokes power. The H7 stops sending edges on a critical
  fault and after 500 ms of stalled application loop while actuating.
- A USB bridge `BOOTLOADER`, `APP`, or `RESET` command first opens PWR2 and
  waits for its feedback to fall. This planned H7 reset does not clear a
  pre-existing latched coprocessor fault.
- The G474 sends a 21-byte `!SF1` status line to the H7 at state changes and
  every 500 ms. The bridge inserts it between complete host command lines in
  application UART mode and never in ROM bootloader mode. The frame carries
  fault bits, state, generation, clear result, and CRC-8. The H7 publishes
  separate `coprocessor_*` telemetry fields and `fault status` prints each
  processor's faults. The H7 publishes `main_fault_*` fields alongside its
  existing fault telemetry. A stale or missing G474 status is identified as
  such.
- READY is checked by the main MCU before actuation. The G474 does not require
  it at power-up because the main MCU deliberately holds the driver in reset
  for more than 500 ms during normal startup.
- The POST here only checks that PWR2 feedback is off. Independent ADC, PWM,
  CAN, thermal, encoder monitoring, a challenge/response watchdog, an internal
  G474 watchdog, and full POST remain open. The heartbeat proves changing H7
  execution, not correct application behavior at every instant.
- The 150 ms power-feedback and 250 ms heartbeat deadlines are provisional.
  They are not measured gate-bias decay or SSO latency claims.
- The G474 has no direct DC-link voltage input in the checked control-board
  netlist. Bus overvoltage detection currently relies on the H7's MAX22530
  input and comparator, so it is not an independent coprocessor monitor.
- The H7 firmware treats throttle and CAN loss as warnings for this interim
  no-throttle bench configuration. DC-link undervoltage is also a warning
  because a disconnected HV bus is normal during bench setup. Encoder loss
  during closed-loop actuation,
  ADC overcurrent, DC-link overvoltage, detected RAM or flash ECC, and
  heartbeat/power faults shut down. RAM ECC is interrupt driven; flash ECC
  status is polled from the H7 application loop. The present defaults are
  500 A and 190 V. Neither threshold alone
  proves protection of 600 A IGBTs or 200 V capacitors without response-time,
  calibration, and transient testing.
- This is a source and host-test milestone. KiCad netlist inspection does not
  replace an energized hardware fault-injection test or scope measurement.
- PC7 is high-impedance during the MCU's hardware reset interval. The gate
  board schematic connects this net directly to U6's input; its reset-state
  off behavior must be measured on hardware before claiming fail-safe startup.

From `RTE/Images/Gen7FW/CoProcessor`, run the host policy and heartbeat tests with
`bash Safety/tests/run.sh`, then build the target with `cmake --preset Debug`
and `cmake --build --preset Debug`.
Run `Safety/tests/verify_pinout.py` from this repository when `kicad-cli` and
the adjacent `InverterGen5` checkout are available.
Run `bash Lib/SafetyFaultLink/tests/run.sh` from RTE for the shared frame codec.
The H7 heartbeat timing test runs with
`bash RTE/Images/Gen7FW/MainProcessor/Safety/tests/run.sh` from the workspace.
