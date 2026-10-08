# Gen7 coprocessor safety hardware test — 2026-09-27

## Setup

- RTE Studio and the current `rte` binary were built before testing. Studio was
  opened on the local display and connected to the Gen7 USB bridge.
- The G474 control port reported application mode at 921600 baud. The H7
  reported the `foc_demo` graph (`bd5390d76c425eb1`). These observations
  establish the running host contract, but do not independently verify the
  exact flashed binary hashes.
- The DC link was about 49–50 V. Motor control stayed `IDLE`,
  `control_outputs_enabled=0`, and `pwm_moe=0` during the first tests. The
  operator authorized fault testing at this voltage.

## Energized observations

| Stimulus | Expected coprocessor behavior | Observed |
| --- | --- | --- |
| H7 `gatepwr 0`, dropping PWR1 feedback | Latch off PWR2 and report main power feedback loss | `FAULT_LATCHED`, `PowerFeedbackLost`, `SAFETY_TRIP=4`, `SAFETY_TRIP_INPUTS=08`, `PWR2_EN=0`. H7 shell and telemetry remained responsive. |
| `fault clear coprocessor` while PWR1 remained off | Accept a guarded clear but keep PWR2 inhibited | G474 reported `clear=accepted`, `SAFETY_STATE=0`, `PWR2_EN=0`. |
| H7 `gatepwr 1` | Rearm after valid heartbeat and both power feedbacks | G474 reported `POWERING` then `ARMED`, `PWR2_EN=1`; H7 `gate_ready=1`. |
| H7 `gaterst 1` then `gaterst 0` | No false coprocessor trip if the shared /FAULT line stays released | G474 remained `ARMED`, faults zero; H7 reported `gate_fault=0`. This did not exercise the /FAULT trip path. |
| H7 `fault test PwmBreak` while idle | H7 removes PWM, reset, PWR1, and heartbeat; G474 opens PWR2 | H7 reported the Critical shutdown, `pwm_moe=0`, outputs disabled. G474 latched `PowerFeedbackLost`, `PWR2_EN=0`, `SAFETY_TRIP=5`, `SAFETY_TRIP_INPUTS=0A`. |
| Two-step `fault clear all` after the Critical trip | Restore coprocessor power first, then recheck gate hardware and rearm the H7 break handler | First clear moved the G474 through `INHIBITED`, `POWERING`, `ARMED`. Second clear reported gate ready, no gate fault, and `MOE=0`. Final H7 and G474 fault sets were empty. |

The trip reason 5 in the Critical test means PWR2 feedback went low while
PWR1 feedback and the H7 heartbeat still read high at the G474 sample. The H7
had already ordered a Critical shutdown. The current policy reports the first
feedback transition it sees, so this test cannot distinguish normal shutdown
timing from a separate PWR2 feedback event. It does confirm that the G474
latched PWR2 off. No fault attribution change was made on this evidence alone.

State after the 49 V tests: G474 `SAFETY_STATE=2` (`ARMED`), `SAFETY_FAULTS=0`, `PWR2_EN=1`;
H7 `control_state=IDLE`, `control_outputs_enabled=0`, `pwm_moe=0`,
`gate_ready=1`, and both fault name fields `none`. Telemetry remained fresh.
The G474's cumulative `RX_DROPPED=5042354` and `UART_ERRORS=220` did not
increase across the observed checks; these historical counters do not establish
the current link's error rate.

## Follow-up DESAT stimulus at 100 V

The operator prepared the same motor-winding setup described in
`../Documentation/Docs/Safety-and-Compliance/Testing/Hardware/Desat-Short-Circuit-Gen7-Size2/Index.md`.
The live bus was 100.3 V and the rotor was stationary. The earlier report's
temporary settings were applied: `Motor.MaxTorqueCurrentA=0` and `ocset 800`.
One `gatefire 0 100 2` pulse completed at 99.9 V with reported phase currents
`+19.94/-68.55/+48.62 A`; `/FAULT` remained released and neither MCU
latched a fault. This pulse therefore did not exercise the G474 `/FAULT`
path. The operator's scope observed about 400 A peak with no supply foldback.
The firmware's frozen 2.5 kHz spike capture measured a sampled peak of
301.5 A, showing why the end-of-command current underreports the pulse peak.
A subsequent `gatefire 0 100 3` pulse also caused no driver or MCU fault;
the scope did not trigger on that attempt, so its peak current is unknown.
The firmware's frozen capture for that first 3 ms attempt recorded a sampled
peak of 323.9 A. A second 3 ms attempt with the scope rearmed again produced
no `/FAULT` or MCU trip. The operator saw about 400 A again, but the scope
probe is rated only to 150 A, so this is qualitative evidence of a large
current pulse, not a calibrated peak measurement.
The 50 A torque setting and 500 A software overcurrent setting were restored
immediately after each attempt; `config get` confirmed 50 A after the first.
The open-loop `stop` command was also issued after the first attempt.

After the clean pulses, `pwm_moe=1` while `control_outputs_enabled=0`, the H7 reset
output was asserted, the gate-driver `/FAULT` and break pins were high, and
both MCU fault sets were empty. MOE alone is not an actuation indication in
this firmware; the gate reset and disabled channel outputs keep the stage off.

The next `gatefire 0 100 3` pulse produced a real DESAT event. The H7 reported
`PwmBreak` with reason `DESAT break (/FLT low)` and shut off PWM, gate reset,
and PWR1. The G474 latched `GateDriver`: `SAFETY_STATE=3`,
`SAFETY_FAULTS=00000001`, `SAFETY_TRIP=3`, `SAFETY_TRIP_INPUTS=0F`, and
`PWR2_EN=0`. The input bits show that PWR1/PWR2 feedback and the H7 heartbeat
were present when the G474 saw `/FAULT` asserted. This is direct energized
evidence for the independent G474 `/FAULT` trip path; response time was not
scope-measured.

The first `fault clear all` after this real trip cleared the main `PwmBreak`
but the G474 reported `clear=refused`, because `/FAULT` remained low with PWR2
off. Releasing and reasserting H7 gate reset (`gaterst 0`, `gaterst 1`) cleared
the driver latch without enabling PWM. `fault clear coprocessor` then returned
the G474 to `ARMED`; a final `fault clear all` passed the gate recheck and
rearmed TIM1 break. Both fault sets were empty and PWM outputs disabled.

The H7 recovery command was fixed to perform this reset release and reassert
automatically on the first `fault clear all` when `/FLT` remains low. It aborts
if `/FLT` stays asserted. The rebuilt main image SHA-256 is
`ab1ef1980a07c40766c41451e76f6ceaec7fefdb66558b94cf7900b770b27b85`;
the MCP flash operation succeeded. The H7 and G474 returned to responsive,
fault-free application mode at about 100 V.

One post-flash `gatefire 0 100 3` pulse reproduced the physical DESAT event:
G474 trip reason 3, input bits `0F`, PWR2 off, and H7 Critical `PwmBreak` with
MOE off. The first `fault clear all` printed that it cleared the gate `/FLT`
latch with outputs held in reset. G474 then reported `INHIBITED`, `POWERING`,
and `ARMED`, with `clear=accepted`. The second `fault clear all` passed the
gate reset/recheck (`ready=Y`, `fault=N`, `MOE=0`) and rearmed TIM1 break.
Final telemetry: both MCU fault sets `none`, G474 `ARMED`, H7 `IDLE`,
`control_outputs_enabled=0`, `pwm_moe=0`, `gate_ready=1`, and `vdc_v` about
99.9 V. `Motor.MaxTorqueCurrentA` was confirmed restored to 50 A; `ocset 500`
was acknowledged after the pulse.

## Tests still requiring an independent input stimulus

- Stop only the H7 heartbeat while PWR1 remains on, then verify G474 reason 2
  within its 250 ms policy deadline and recovery after heartbeat resumes.
- Independently interrupt or hold PWR2 feedback to verify reasons 5 and 6,
  and measure actual PC7/PWR2 response time with a scope. A software-injected
  H7 Critical fault does not isolate those inputs.
- Check reset-state behavior of PC7 on hardware before claiming fail-safe
  power-off during a G474 hardware reset.

The available USB and shell controls do not provide those independent input
stimuli. No SWD probe was connected during this test.

## Software checks

- G474 safety policy and heartbeat tests: passed.
- G474 USB bridge DMA tests: passed.
- H7 safety-link tests: passed.
- Shared fault-frame codec tests: passed.
- Host integration selection
  `RTECLI_mcp_flash_integration|RteCli|RTEStudio_session_stale`: 4 passed.
- Live MCP contract checks: `rte_device_commands` discovered 53 H7 commands,
  `rte_signal_info` found five live `coprocessor_*` signals, and
  `rte_build_info` decoded the running graph identity and manifest counts.

These host tests exercise policy and protocol behavior; they do not replace the
remaining energized input and timing tests above.
