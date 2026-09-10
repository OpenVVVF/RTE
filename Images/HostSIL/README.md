# HostSIL — firmware-in-the-loop SIL simulator

> Overview of both simulators and when to use which:
> [docs/simulation.md](../../docs/simulation.md).

`host_sil` compiles the **real Gen6FW application code, unmodified**, for the
host (Linux) and runs it against the HostSim ODE PMSM plant on a simulated
clock: software-in-the-loop with the firmware's own init sequence, TIM1 ISR
control path, and app loop.

```
cmake -S Images/HostSIL -B build/hostsil_build
cmake --build build/hostsil_build -j
cd Images/HostSIL && ../../build/hostsil_build/host_sil scenarios/sil_foc_demo.json --realtime 0
python3 scripts/validate_trace.py sil_foc_trace.csv --control-start-s 1.6 --iq-a 8
```

### Live telemetry link (`--live`)

```
cd Images/HostSIL && ../../build/hostsil_build/host_sil scenarios/sil_foc_demo.json --live
build/bin/RTEStudio --tcp 127.0.0.1:14608 --protocol ivp        # from repo root
```

With `--live`, `host_sil` proxies the firmware's **own** USART3 TX byte stream
(the COBS-framed InverterProtocol packets produced by `Telemetry.cpp`,
tapped in `HAL_UART_Transmit_DMA`) verbatim onto a TCP socket — the same
stream RTEStudio decodes from real hardware, produced here by the real
firmware at its own rates (100 Hz DATA; DEFINEs re-announced at 10 Hz, so a
client joining mid-run has the full key table within ~100 ms).
`--live` implies `--realtime 1.0` unless `--realtime` is given explicitly;
`--port P` selects the listen port (default **14608**, same as HostSim).
Without `--live` no socket is opened and batch behavior is unchanged.

Client→server bytes — the RTEStudio text console (`IvpTcpClient::SendLine`
appends `'\n'`) or any raw TCP client — are forwarded **verbatim** into the
firmware's USART3 IT-RX path: the SIL HAL models the single-byte
`HAL_UART_Receive_IT` arm / `HAL_UART_RxCpltCallback` interrupt pairing the
Gen6FW `CommandShell` expects, delivering queued client bytes on the
scheduler context while the firmware is blocked (one byte per callback, the
cooperative stand-in for the hardware RXNE IRQ).  Shell commands typed over
the link therefore take the real firmware code path, exactly as minicom on
hardware.  Both `'\n'` and `'\r\n'` line endings work (the shell treats
either as a terminator; empty lines are ignored).  Without `--live` no
socket is opened, no RX bytes ever arrive, and batch behavior is unchanged.
`scripts/ivp_probe.py` is a stdlib-only command-line probe that decodes the
stream and prints the firmware keys (handy without a GUI);
`scripts/shell_client.py` sends command lines and prints the shell's
`print`-key responses.

Note the live link ends when the scenario `simulation.duration_s` elapses;
use a longer-duration scenario for interactive sessions.

The configure step auto-emits the firmware tree
(`build/hostsil_fw_src` = copy of `Images/Gen6FW/` + graph-generated
`generated/` domain code) via `RTECodeEmitter`.  Re-emit after a graph change
with `scripts/emit_firmware.sh [graph.json] [output-dir]`; select a different
graph at configure time with `-DSIL_GRAPH=<path>` and a fresh `-DSIL_FW_SRC`.

## Architecture

* `sil/stm32shim/` — minimal STM32H7 HAL + CubeMX headers (`stm32h7xx_hal.h`,
  `main.h`, `adc.h`, `tim.h`, `spi.h`, `usart.h`, `fdcan.h`, `gpio.h`,
  `dma.h`) shadowing the real ones via include-path order.  HAL_GetTick /
  HAL_Delay are backed by the simulated clock.
* `sil/sil_rt.*` — cooperative two-context runtime.  The firmware runs on its
  own thread and blocks only in `HAL_Delay` (time-wait) and in
  `EncoderADC::diagnose()` (app-loop rendezvous, once per `loop()` pass).
  The scheduler advances time, steps the plant, and fires the sensor/ISR
  hooks only while the firmware is blocked — contexts never run
  concurrently.
* `sil/sil_world.*` — the shared physical world: `hostsim::OdePlant`, DC-link
  voltage, pole voltages, throttle pins.
* `sil/sil_*.cpp` — driver shims implementing the firmware's own driver
  classes (`PWM`, `PhaseCurrentADC`, `EncoderADC`, `MAX22530`,
  `ApplicationSensors`, `CanBus`, F-RAM, UART, …) against `silWorld()`.
* `sil/sil_live_server.*` — the optional `--live` TCP server proxying the
  firmware's COBS-framed UART telemetry stream to RTEStudio (TX) and
  forwarding client-sent command bytes into the modeled huart3 IT-RX path
  (`silUartRxEnqueue`, drained by `silUartRxPoll` in `sil_hal.cpp`).
* `src/main.cpp` — scenario parsing, scheduler, trace CSV.

### Per-TIM1-update-event order (hardware order)

1. plant step with the latched duties (averaged-duty ODE model),
2. injected phase-current conversion-complete (the ADC ISR; the generated
   `adc_isr` step reads the *previous* sample, exactly like the real ISR),
3. encoder sample (TIM2 10 kHz free-run, or TIM1-synced while control runs),
4. `HAL_TIM_PeriodElapsedCallback` → `LoopStats::tim_isr++`, graph
   `app::TimIsrStep` (gated on `ControlSupervisor::isRunning()`), legacy
   `FocControlManager_OnPwmPeriod`, open-loop SPWM ramp.

App-loop (`InverterMain::loop()`) iterations run at `simulation.app_loop_hz`
(default 1000 Hz): supervisor service, calibrators, shell, telemetry.

### Fault-injection scenarios

The scenario JSON accepts a top-level `faults` block that drives the modeled
*sensor/actuator surface* the firmware reads — the firmware itself is never
touched, so every trip is raised by real firmware code paths (with the
hardware driver's verbatim logic where a SIL shim replaces the driver file).
Each fault is a window `[time_s, time_s + duration_s)`; `duration_s <= 0`
latches to end of run; `time_s < 0` (the default) disables it.  A companion
top-level `commands` block (`{"<time_s>": "<shell line>"}`) feeds any
firmware shell command through `CommandManager::processLine` at the given sim
time — e.g. arming a protection threshold before injecting its trip, or
starting legacy FOC control.

| faults key | what it models | firmware trip path |
|---|---|---|
| `vdc_glitch_time_s` / `vdc_glitch_v` / `vdc_glitch_duration_s` | DC-link sag seen by the MAX22530 channel 0 sense divider (also collapses plant drive voltage) | with the UV comparator armed (`maxcfg_uv <V>` shell command): `MAX22530::update` (shim, register-level port) latches `INT_CO_NEG_1` → `FaultManager.raise(Max22530Uv, Max22530Undervoltage)` [Critical] |
| `oc_inject_time_s` / `oc_inject_a` / `oc_inject_duration_s` / `oc_inject_phase` (0=U,1=V,2=W) | current spike added at the ADC *counts* level (saturated/glitched channel; plant stays physical) | `PhaseCurrentADC::onInjectedConversionComplete` software-OC check (verbatim port; 3 consecutive samples > `m_oc_threshold_a`, 500 A default, `ocset` to change) → `PhaseOvercurrent / PhaseOvercurrentSoftware` [Critical]. The ADC analog-watchdog AWD path is not modeled in SIL. |
| `encoder_freeze_time_s` / `encoder_freeze_duration_s` | encoder sample stream stalls (no DMA completions at all) | legacy FOC path (`foc start`): `FocControlManager::onPwmPeriod` sample-age check (`HAL_GetTick() - lastSampleMs() > ENCODER_STALE_MS=5`) → `EncoderTimeout / EncoderSampleTimeout` [High] + safe stop. (The graph-control path has no firmware staleness check — see caveats below.) |
| `encoder_loss_time_s` / `encoder_loss_duration_s` | sin/cos outputs collapse to the 32768 bias mid (excitation loss) | `EncoderADC::diagnose` amplitude-collapse check (verbatim port, 25 consecutive < 500 counts EMA) → `EncoderAmplitude / EncoderAmplitudeLow` [Warning: latched, does not stop the drive] |
| `temp_spike_time_s` / `temp_spike_c` / `temp_spike_channel` (0..2 board, 3 motor) / `temp_spike_duration_s` | temperature channel driven to a value, round-tripped through the modeled sensor curve + divider (KV-configurable `Hw.Temp.Bx.*` / `Motor.Temp.*`, same keys/defaults as hardware) | ported `ApplicationSensors` evaluation: rail → `TempSensor` (Warning); over `CritC` sustained 500 ms with 5°C hysteresis → `OvertemperatureMotor` / `OvertemperatureInverter` (Critical). Board channels are disabled by KV default (enable via `firmware_config` `Hw.Temp.Bx.En: 1`). |

Run the shipped demonstrations:

```
host_sil scenarios/sil_fault_injection.json       # encoder-loss Warning + overcurrent Critical (graph FOC, demo baseline)
host_sil scenarios/sil_fault_encoder_stall.json   # encoder stream stall -> EncoderTimeout (legacy `foc start`)
host_sil scenarios/sil_fault_undervoltage.json    # Vbus sag -> Max22530Uv (arms UV via shell first)
host_sil scenarios/sil_fault_overtemp.json        # motor 200 C -> OvertemperatureMotor
host_sil scenarios/sil_fault_none.json            # faults present but disabled: trace matches sil_foc_demo byte-for-byte
```

Batch runs log every fault edge twice: host-side `[SIL] t=… fault
raised: source=… severity=…` at the sim µs tick and the firmware's own
`[FW t=…] [FAULT][sev][category] Name triggered: reason` line (console
mirror, below); the end-of-run summary lists the full trip history.

### Firmware console mirror in batch mode

`sil/sil_fw_console.*` taps the firmware's USART3 TX byte stream (the same
COBS-framed InverterProtocol packets that `--live` proxies to TCP) and walks
it with the shared Lib/InverterProtocol decoder; complete `print`-keyed
strings land on stdout as `[FW t=…]` lines.  Boot chatter, shell responses
and — most importantly — the firmware's own
`[FAULT][C][category] Name triggered: reason` / `[SUP]` / `[SAFETY]` messages
are therefore visible in batch runs, which previously only showed `[SIL]`
host-side lines.

### Control start

After boot (~1.3 s of sim time — the hardware `HAL_Delay` boot sequence runs
in simulated time), the harness posts the equivalent of the shell commands
`config set/save <key> <value>` (scenario `firmware_config` seeds),
`control start`, and `var set IqVar/IdVar <amps>` (scenario `control`)
through the firmware's own `CommandManager::processLine`.  All of it executes
on the firmware thread.

## Fidelity notes / deliberate simplifications

* **Cooperative scheduling**: ISRs run at tick boundaries; they never preempt
  mid-instruction.  Firmware `__disable_irq()` regions are therefore
  trivially safe in SIL (they model a stronger guarantee than hardware).
* **TIM1 event rates follow the firmware's own bookkeeping**
  (`pwm_switching_freq_hz` / `pwm_update_freq_hz`), i.e. the same values that
  drive the control `dt` — the register-derived rates (`ARR/PSC/RCR`) are
  modeled for register-level fidelity only.  Boot defaults: 2.5 kHz
  switching / 5 kHz update in FOC mode (RCR=0 dual update).
* **One injected ADC burst per switching period**, sampled perfectly clean
  (averaged duty, no switching ripple, no dead-time effects).  TIM1-OC4
  adaptive trigger placement is bookkeeping-only.
* **Sensors are ideal** apart from quantization and the documented LA37S600
  polarity inversion; encoder sin/cos is centered at 32768/30000 counts with
  one cycle per mechanical revolution.
* **CAN/UART**: CAN frames are accepted and dropped; telemetry TX DMA
  completes at the next app tick.  The telemetry bytes themselves (COBS
  InverterProtocol) are forwarded verbatim to TCP clients in `--live` mode
  and discarded otherwise.  In `--live` mode client-sent bytes enter the
  modeled huart3 IT-RX path: bytes queue in a host-side FIFO and are
  delivered one per `HAL_UART_RxCpltCallback` at app-tick boundaries — the
  same bytes and callback pairing the shell sees on hardware, coalesced to
  tick cadence instead of per-byte preempt timing.  FRAM is a 256 KiB
  in-memory image (optional file backing via scenario `fram_image`).
* The firmware's `platform_micros()`/DWT paths see cycles = sim_us *
  550 MHz.
* **Fault-injection boundaries**: the modeled fault surface is the sensor
  world (phase-current ADC counts, MAX22530 channel voltages + comparator
  windows, encoder sin/cos stream, temperature channels) plus DC-link level.
  Fault classes rooted in effects the shims don't model stay untrippable in
  HostSIL: the ADC *hardware* analog watchdog (`AdcWatchdog` — SIL documents
  `configureAnalogWatchdog()` as a no-op), MAX22530 SPI CRC/framing/DMA and
  field-side loss (`Max22530Comm/Adc/Field`), gate-driver DESAT break
  (`PwmBreak`), UVLO (`GateDriverUvlo` is modeled by the GPIO shim and always
  healthy), supply-rail PVD/AVD/VOSRDY, CAN bus-off/error-passive, FRAM
  errors.  On the graph-control path the firmware currently has **no**
  encoder-staleness or Vdc sanity check — those live in the legacy
  `FocControlManager` (used by `foc start`), which is why
  `sil_fault_encoder_stall.json` runs that path.
