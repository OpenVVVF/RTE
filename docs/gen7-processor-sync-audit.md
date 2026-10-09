# Gen7 processor synchronization audit

This source audit follows the commanded-reset fix in `55bc895`. It covers
PWR1/PWR2 ownership, PD8 heartbeat, guarded PD9 clears, gate reset, generated
control, native FOC/open-loop startup, calibration gate startup, UART safety
status, and Studio/MCP reporting. Initial verification used host tests and
target compilation. The subsequent connected-device flash and monitoring
check is recorded below; actuation remains untested.

## Confirmed issues and fixes

| Event | Previous behavior | Updated behavior |
| --- | --- | --- |
| Last main Critical fault cleared through an individual/main scope | Critical shutdown had stopped PD8; clearing the latch reset the shutdown flag without resuming PD8. A later start could fail against a coprocessor that still considered H7 dead. | Clearing the final Critical latch resumes PD8. Gate reset, PWM, and PWR1 remain off until explicit recovery/start. Clearing a warning or only one of several Critical faults does not resume PD8. |
| G474 opens PWR2 or its private status link becomes stale while H7 still runs | G474 reports were diagnostic. Generated control could remain RUNNING until /RDY decay plus its 100 ms debounce; native paths depended on their own pin polling. | A fresh, fault-free ARMED report is required for start. Loss of that report during actuation raises main Critical CoprocessorSafety and synchronizes local shutdown and controller states in the application loop. |
| Open-loop or direct/static calibration PWM followed by a stalled H7 loop | The heartbeat's active-loop deadline only included graph control and native FOC flags. SysTick could keep declaring H7 alive while another output path stalled. | The heartbeat includes open-loop requests and the local channel-enable/MOE/PWR1/reset combination. Idle measurement MOE alone does not count as actuation. |
| Critical fault during asynchronous native startup or calibration warmup | FOC/open-loop advanced startup before checking Critical faults. Software could retain a starting/running state after the hardware shutdown. Calibration warmup could release gate reset after a new fault. | Startup checks faults before advancing; Critical shutdown stops native paths and marks the graph supervisor FAULT immediately. Calibration hardware refuses startup/advance without safe main faults and fresh G474 readiness. Generated startup also rechecks faults after its blocking gate sequence and before PWM start. |
| New G474 clear request or stale G474 status with otherwise live H7 telemetry | The previous clear result remained visible until a response arrived. Old G474 fault values were republished while the main status endpoint could still say fresh. Studio's diagnostics dialog also used raw last values. | Clear requests show pending/timeout until a newer consistent result arrives. Firmware marks stale G474 values unknown. Studio and MCP share freshness checks, expose independent coprocessor freshness, and return null expired fault/power values. Power and heartbeat enable states are published explicitly. |

The new main source is `CoprocessorSafety`, bit 38. Reasons distinguish a
coprocessor trip, non-armed power state, and unavailable/stale status. Original
G474 fault flags remain separate; no new G474 command or wire-frame field is
needed. The existing extended fault string represents bit 38 as
`0x00000000,1:0x00000040` when it is the only main fault.

## Expected behavior retained

- `control stop`, `foc stop`, and calibration shutdown disable actuation while
  leaving measurement/telemetry running. They do not intentionally cut PWR1;
  the coprocessor should remain armed without a power-loss fault.
- Critical H7 shutdown and unexpected reset really remove power/heartbeat.
  G474 feedback/heartbeat faults are an expected secondary safety response.
  The original main fault remains the root diagnostic.
- Shell `gatepwr` and `gaterst` explicitly advertise fault injection and bypass
  the fault manager. Their effects must remain visible and may trigger safety
  faults; they are not normal stop commands or planned-reset exemptions.
- `fault clear all` retains the guarded two-stage recovery when G474 has
  latched PWR2 off. An accepted clear does not itself start a motor.
- A private status-link timeout while idle blocks start and reports unknown
  status; it does not manufacture an active-actuation fault while idle.

## Verification

- Built current `rte` and `RTEStudio`.
- Built generated H7 Release firmware from `Assets/Examples/foc_demo.json`
  and the updated Gen7 main base in `build/safety-sync-fw`.
- Ran `Images/Gen7FW/MainProcessor/Safety/tests/run.sh`: heartbeat/direct PWM,
  ECC, status/clear tracking, and integrated fault-manager/calibration tests.
  The integration uses the real safety link, fault manager, frame consumer,
  and calibration hardware with mocked GPIO and controller callbacks.
- Ran G474 safety/reset and bridge command/DMA tests, and shared safety-frame
  codec tests.
- Ran the required host test filter:
  `RTECLI_mcp_flash_integration|RteCli|RTEStudio_session_stale`.
  The Studio test includes independent private-link freshness, host age,
  unknown power values, and preserved timeout reporting.

## Deployment and hardware follow-up

Rebuild/reflash the **H7 main MCU** for this audit's firmware changes. Rebuild
and restart **RTE Studio/rte** for the shared status report. The existing G474
`!SF1` contract is compatible; the earlier commanded-reboot fix separately
requires its updated **G474 coprocessor** image. Both images were subsequently
flashed as recorded below.

On a connected device, check `rte_device_commands`, filtered `rte_signal_info`,
`rte_build_info`, and `rte_control_status`, then verify:

1. Normal stops leave TIM1 measurement live, local output enable off, and G474
   ARMED without a new power-loss fault.
2. A G474 trip during graph/native operation promptly stops H7 actuation and
   reports the separate G474 reason plus main CoprocessorSafety.
3. Individual/main-only clears restore heartbeat monitoring without restarting
   outputs; guarded two-stage recovery still succeeds.
4. A missing private G474 report becomes stale independently of live H7
   telemetry, blocks start, and stops active operation within the 1500 ms
   status deadline plus application-loop service latency.
5. Native startup interrupted by a Critical fault never resumes after clear;
   a new explicit start is required.

Host simulations and successful target compilation do not measure physical
shutdown latency, reset feedback pulse visibility, or UART delivery under
hardware faults. The 1500 ms status deadline preserves the existing H7
freshness threshold; it is not a new safety timing qualification.

## Connected-device flash and monitoring, 2026-10-09

- Flashed and verified the G474 first through USB DFU, then the H7 through
  the updated Gen7 bridge. Both applications restarted successfully.
- G474 image: `Images/Gen7FW/CoProcessor/build/Debug/CoProcessor.elf`, SHA-256
  `763855632dc8383bea99957b1588c46e5f39929c274cb275ebedc61601aba75a`.
- H7 image: `build/safety-sync-fw/STM32CubeMX.elf`, SHA-256
  `31f20a2bc493b0a2b1fabc74d344338b7465980deb9b8ff5d7ba869c8e9d122b`.
- The rebuilt Studio received live telemetry. `rte_build_info` confirmed
  `foc_demo`, graph hash `bd5390d76c425eb1`, 82 nodes and 33 manifest signals.
  `rte_device_commands` discovered and verified all 53 registered commands;
  filtered signal discovery and control status exposed the new safety fields.
- The user confirmed the DC bus supply was off. A live voltage snapshot read
  approximately 1.32 V rather than exactly zero. H7 reported Critical
  `Max22530Uv`; G474 separately reported latched `PowerFeedbackLost`.
- Local and generated outputs and gate power were off, gate reset was
  asserted, and the heartbeat was disabled by the Critical shutdown. TIM1
  measurement and telemetry remained running. Main and coprocessor status
  were both fresh.

Faults were not cleared and motor control was not started. This check verifies
flashing, communication, identity, and shutdown reporting with the bus supply
off; powered recovery and the clean-state reboot regression remain pending.
