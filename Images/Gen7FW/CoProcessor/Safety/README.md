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
switches must be on for the +12 V gate supply. Each feedback net is fed from
its switch output through a 3 kΩ/1 kΩ divider, a 120 Ω series resistor, and
a 3 V clamp. Main MCU PD5 also connects to
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
  When this latch has opened PWR2, the H7 `fault clear all` path requests the
  G474 clear with gate outputs held in reset. If a real DESAT left /FAULT low,
  the H7 briefly releases and reasserts gate reset while PWR2 and PWM remain
  off, and aborts recovery if /FAULT stays low. After the G474 reports `ARMED`,
  a second `fault clear all` rechecks gate hardware and re-arms TIM1 break.
- Control-port `STATUS` retains `SAFETY_TRIP` and `SAFETY_TRIP_INPUTS` when a
  fault latches, plus the current `PWR2_EN` command. Trip values 4, 5, and 6
  distinguish loss of the main switch feedback, loss of the second switch
  feedback after arming, and second-switch startup timeout. Input bits are
  PWR2 feedback (1), PWR1 feedback (2), /FAULT asserted (4), and live H7
  heartbeat (8). These fields diagnose a feedback fault without changing the
  existing `!SF1` UART frame or the power-off policy.
- A USB bridge `BOOTLOADER`, `APP`, or `RESET` command, or a shell `reboot`
  sent through the application bridge, first opens PWR2. The bridge recognizes
  a complete, case-insensitive `reboot` line, including across USB packets and
  TX-ring wrap, and withholds its terminating DMA chunk until the main loop
  has prepared the safety policy. Extra arguments and overlong lines rejected
  by the H7 do not start a planned reset; ROM bootloader bytes stay opaque.
  These resets preserve any pre-existing coprocessor fault latch.
  PWR2 feedback must fall within 150 ms or `PowerStuckOn` latches. A shell
  reboot must also drop PWR1 feedback within 1500 ms or `MainHeartbeatLost`
  latches, keeping PWR2 off if the H7 never executes the command. Direct bridge
  resets record the NRST assertion because their blocking pulse can hide the
  PWR1-low interval from polling. After reset is observed and PWR2 feedback is
  off, ordinary arming requires two fresh H7 heartbeat edges, PWR1 feedback,
  and released /FAULT. Old heartbeat edges cannot rearm PWR2 while a reboot
  waits in the H7 shell. Time spent unpowered in startup or the ROM bootloader
  does not create a power-loss fault. This fix requires only a G474 coprocessor
  reflash; H7 commands, USB identity, UART modes, and `!SF1` framing are unchanged.
- The G474 sends a 21-byte `!SF1` status line to the H7 at state changes and
  every 500 ms. The bridge inserts it between complete host command lines in
  application UART mode and never in ROM bootloader mode. The frame carries
  fault bits, state, generation, clear result, and CRC-8. The H7 publishes
  separate `coprocessor_*` telemetry fields and `fault status` prints each
  processor's faults. The H7 publishes `main_fault_*` fields alongside its
  existing fault telemetry. A stale or missing G474 status is identified as
  such.
- USART3 RX uses an 8 KiB circular DMA buffer. The bridge copies bounded
  chunks to USB, counts overwritten bytes in `RX_DROPPED`, and uses only
  half/full-buffer DMA interrupts rather than an interrupt per UART byte.
  USB-to-UART traffic uses DMA1 channel 2 in contiguous blocks, ending app
  mode blocks at command-line boundaries so `!SF1` can still be inserted
  between commands. The ROM bootloader's binary stream stays opaque at
  460800 8E1; application mode remains 921600 8N1. DMA1 channels 1 and 2
  and DMAMUX requests are configured in `usbd_cdc_if.c`; preserve this setup
  when regenerating CubeMX files. This coprocessor-only change needs a G474
  reflash. The safety input polling loop remains active at its prior speed.
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
- The H7 classifies a latched throttle A/B plausibility fault as Critical;
  that severity was restored after the initial G474 safety merge. The
  analog detection threshold remains 10% sustained for 100 ms, while FSR-01
  requires a trip above 5% and independent detection on both MCUs. A
  software fault injection confirmed the H7 safe-state action, but physical
  dual-throttle behavior has not been verified. CAN loss remains a warning
  in this interim bench configuration. The MAX22530 critical-UV comparator
  fault is Critical; the lower UV derate threshold is a warning. Encoder loss
  during closed-loop actuation,
  ADC overcurrent, DC-link overvoltage, detected RAM or flash ECC, and
  heartbeat/power faults shut down. RAM ECC is interrupt driven; flash ECC
  status is polled from the H7 application loop. The H7 initializes its used
  RAM banks at reset before clearing startup ECC flags; the unused ITCM
  monitor is excluded. An ECC trip after startup latches the H7
  power stage off until reset while leaving a diagnostic shell and telemetry
  path available; `fault clear` refuses to clear it. The present defaults are
  500 A and 190 V. Neither threshold alone
  proves protection of 600 A IGBTs or 200 V capacitors without response-time,
  calibration, and transient testing.
- This is a source and host-test milestone. KiCad netlist inspection does not
  replace an energized hardware fault-injection test or scope measurement.
- PC7 is high-impedance during the MCU's hardware reset interval. The gate
  board schematic connects this net directly to U6's input; its reset-state
  off behavior must be measured on hardware before claiming fail-safe startup.

From `RTE/Images/Gen7FW/CoProcessor`, run the host policy and heartbeat tests with
`bash Safety/tests/run.sh` (including normal H7 warmup, planned resets, fresh
heartbeat reacquisition, and reset failure deadlines), then build the target with `cmake --preset Debug`
and `cmake --build --preset Debug`.
Run `bash USB_Device/App/tests/run.sh` for DMA wrap, command boundaries, and
reboot recognition across fragmented lines.
Run `Safety/tests/verify_pinout.py` from this repository when `kicad-cli` and
the adjacent `InverterGen5` checkout are available.
Run `bash Lib/SafetyFaultLink/tests/run.sh` from RTE for the shared frame codec.
The H7 heartbeat timing test runs with
`bash RTE/Images/Gen7FW/MainProcessor/Safety/tests/run.sh` from the workspace.
