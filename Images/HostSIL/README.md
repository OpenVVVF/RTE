# HostSIL — firmware-in-the-loop SIL simulator

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
* **CAN/UART**: frames are accepted and dropped; telemetry TX DMA completes
  at the next app tick.  FRAM is a 256 KiB in-memory image (optional file
  backing via scenario `fram_image`).
* The firmware's `platform_micros()`/DWT paths see cycles = sim_us *
  550 MHz.
