# Gen7 Bringup State (2026-09-21)

## Where things stand

Firmware on `coproc-flash-fixes` (HEAD 418bd3b), flashed to the main MCU.
Coprocessor firmware unchanged. Board: Gen7 control board + NCD57100 gate
driver module (2nd/known-good module installed), no motor connected,
DC bus at 0 V.

### Working (verified live)
- DC-link voltage: MAX22530 AIN4 (ch index 3) — was wired to AIN1/phase W.
- DC-link current + power telemetry (`dclink_i_a`, `dclink_p_w`) — ref
  plausibility window was wrong (2.0-3.0 V -> 1.4-1.9 V ADC-side).
- Temp channels: ADC3 PCSEL bits for PF8/PF4 were never set -> floating-mux
  garbage. Board temp config: RTS103C1R2M6L201 = NTC-beta, R25 10k, Beta 3977
  (`Hw.Temp.Bx.*`), KTY84-130 for motor (`Motor.Temp.*`, R25 603).
  NOTE: B3.Beta was set in live FRAM; motor temp sensor reads ~0.15-0.19 V
  (plausible for 10k pull-up vs ~600 ohm sensor); temp sense 2/3 have crimp
  issues per hardware inspection.
- Open-loop start/stop runs without latching /FLT (skip-reset patch).
- `gatefire <phase 0-2> <duty 0-100> <ms>` per-half-bridge pulse tool.

### RESOLVED 2026-09-21 ~03:00: control board + firmware verified end-to-end
Root causes found (all fixed, committed through 86a6c4a):
1. Coproc held gate RESET push-pull LOW forever -> driver never woke.
2. Main firmware never called GateDriver_Init() -> never powered/released.
3. OpenLoopController::init re-asserted RESET after release (fixed).
4. Any reset pulse to a healthy driver latches /FLT ~10-100ms later
   (workaround: skip reset when healthy, all paths).
5. TPS389006 supervisor NIRQ is wired to the gate RESET net (schematic);
   kept asleep via POWERMON_SLEEP=PD6 driven LOW (SLEEP is ACTIVE LOW,
   internal 100k pulldown).
Verified: 'control start' STARTED/RUNNING/STOPPED clean with the gate
module UNPLUGGED; PWM confirmed at connector during gatefire windows
(gatefire 0 90 50; note CHx/CHxN swap: schematic PHASE_U_HIGH=PE8 carries
TIM1_CH1N, so duty>50% on phase 0 shows on the U_LOW pin).
status now prints RESET(PD5)/POWER(PC10)/SLEEP(PD6) ODR states.

### The one open blocker: BOTH gate driver modules dead
`control start` fails: "TIM1 MOE not active after PWM start".
Diag (ControlSupervisor): PE15=0 AND PC11=0 at failure (net genuinely low,
not a read/marginal issue), PC12(/RDY)=1, TIM1_AF1=0x01 (pin-sourced break,
no COMP), BIF set. /FLT goes low within ~us-ms of PWM_Start, /RDY stays
high, identical on TWO different driver modules, supervisor asleep
(POWERMON_SLEEP=high), 0 V bus.

Ruled out: desat-by-bus-voltage (0 V bus; DESAT pin = 1.4 V vs emitter,
below threshold), supply sag on -9 V rail (scoped clean during pulse),
module defect (two modules identical), floating fault net (both pins read 0),
supervisor NIRQ (slept via PD6; also it drives RESET, not /FLT),
encoder (unrelated warning; never gates switching).

Next measurements (scope at the gate module, ref to module secondary GND):
1. Gate-emitter of one switch during `control start` — does the driver even
   try to switch? Gates move + /FLT drops -> driver protection really fires.
   No gate movement -> PWM not reaching module; check control-board PWM path
   to the gate connector (ioc/code pin desync!).
2. /FLT at the driver on the same trigger — which edge trips it.
3. If gates move: capture DESAT pin vs local VEE at the first edge.

Suspicion to check when fresh: PWM routing to the gate-driver connector
(the ioc and code are desynced from the board; verify TIM1 CHx/CHxN land on
the connector's PH_x_HIGH/LOW pins per schematic).

### Flashing ritual
- RTE Studio must be CLOSED (it holds /dev/ttyACM0 -> fake RDP errors).
- `export STM32_PROGRAMMER=/opt/st/stm32cubeclt_1.21.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI`
- Build: `cd Images/Gen7FW && ./build-main-rte.sh`
- Flash: `cd Images/Gen7FW/MainProcessor && ./flash-main.sh` (auto BOOTLOADER,
  download, verify, GO, APP).
- After flashing the main app may not boot on its own (reset-line quirk):
  full inverter power cycle fixes it (coproc RESET command does not).
- Shell: 460800 8N1 on the bridge interface (usb-...if00), prefix first
  command with \r or send twice (first char eaten). Commands: status,
  clearfault, control start/stop/status, start/stop <f> <m>, gatefire,
  temp, dclraw, maxcfg_raw, config get/set.

### Other known issues
- Encoder stuck at rail (unconnected) — blocks FOC cal later.
- Physical 4.7-10k pull-ups on /FLT and /RDY at the gate connector
  recommended (board has none; firmware enables weak internal pulls).
- Open question: why a RESET pulse to a powered NCD57100 faults it
  (worked around by skip-reset-when-healthy; possibly related to the
  supervisor interaction or module reset circuitry).


## Update 2026-09-21 ~03:30 - module-side blocker confirmed
Both NCD57100 modules: rails good (+15/-9/3.353V), desat ~1.5V vs emitter
(normal at 3V bus), RESET released, /RDY=3V, receive valid PWM at the
connector in a clean /FLT window - gates never switch, /FLT re-latches at
idle. Desat zeners lifted/reinstalled - not the cause. Control board
exonerated (clean run with module unplugged; gd_fault=N with module
unplugged = board does not clamp /FLT).
NEXT: bench spare module - all 6 PWM inputs grounded, bench supply, RESET
released, watch /RDY+/FLT and one gate. If dead: loupe EVERY reworked part
on both modules (a wrong-part batch already slipped in once - desat zeners).
