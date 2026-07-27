# Inverter Firmware Base Image — Codegen Integration Plan

## 1. Goal

Turn the current STM32H723 inverter firmware into a **safe base image** that is
completed by a code-generation node tool.  The base image owns:

* Hardware initialization (CubeMX generated code, clocks, GPIO, DMA, interrupts).
* Safety envelope (fault manager, safety actions, supply monitoring, watchdogs).
* Communication transport shells (UART shell/telemetry, CAN error hooks).
* Base command registration and dispatch.
* Example modulation/control paths that show the intended integration pattern.

Codegen fills in:

* The modulation strategy (SPWM, SVPWM, SHEPWM, DPWM, six-step, …).
* The control law (open-loop, FOC, MPC, DTC, …).
* The application layer (throttle mapping, CAN protocols,
  state machines, command tables, telemetry variables).
  (Temperature monitoring is now implemented in the base image:
  `TemperatureSensors` driver + `hw.temperatures` node template, see §7.6.)

## 2. What was done in this pass

* Made a full copy of the firmware into
  `/home/aidan/Desktop/InverterFirmwareBaseImage`.
* Added `/* TIME_DOMAIN: <name> … CODEGEN: … */` markers at every time-domain
  boundary and every codegen insertion point.
* Did **not** implement any new functionality — only identification and planning.

## 3. Time-domain map

The markers fall into four major domains.

### 3.1 Hard-real-time PWM / control ISR domain

These run in interrupt context and must complete before the next PWM update.
They are the codegen replacement targets for modulation and the current/control
loop.

| Marker | File | Line | Description |
|--------|------|------|-------------|
| `TIM1_PWM_UPDATE_ISR` | `Src/stm32h7xx_it.c` | 243 | TIM1 update interrupt vector. |
| `TIM1_PWM_UPDATE_ISR / MODULATION_TIME_DOMAIN` | `Src/Inverter/Drivers/PWM/pwm.c` | 310 | `HAL_TIM_PeriodElapsedCallback`; dispatches FOC or SPWM ramp. |
| `CLOSED_LOOP_MODULATION_START` | `Src/Inverter/Drivers/PWM/pwm.c` | 215 | `PWM_EnableFocMode()` — switches ISR to closed-loop path. |
| `OPEN_LOOP_MODULATION_START` | `Src/Inverter/Drivers/PWM/pwm.c` | 266 | `PWM_StartSPWM()` — switches ISR to open-loop ramp. |
| `PWM_SYNCHRONOUS_CONTROL_ISR` | `Src/Inverter/Control/FocControlManager.cpp` | 428 | `FocControlManager::onPwmPeriod()` — FOC body. |
| `PWM_SYNCHRONOUS_CONTROL_ISR (C linkage dispatch)` | `Src/Inverter/Control/FocControlManager.cpp` | 635 | `FocControlManager_OnPwmPeriod()` bridge. |

### 3.2 Sensor / ADC / communication ISR domain

These are medium-to-background priority ISRs that sample inputs or move bytes.
They feed the control loop and telemetry.

| Marker | File | Line | Description |
|--------|------|------|-------------|
| `ADC_PHASE_CURRENT_ISR (entry vector)` | `Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp` | 386 | `ADC_IRQHandler`. |
| `PWM_SYNCHRONOUS_CURRENT_SAMPLE_ISR` | `Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp` | 396 | Injected conversion complete; converts U/V currents. |
| `ADC_ANALOG_WATCHDOG_ISR` | `Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp` | 408 | Hardware overcurrent watchdog trip. |
| `ENCODER_DMA_ERROR_ISR` | `Src/Inverter/Drivers/Sensors/EncoderADC.cpp` | 411 | Encoder DMA error. |
| `ENCODER_DMA_ISR (entry vector)` | `Src/Inverter/Drivers/Sensors/EncoderADC.cpp` | 421 | `DMA2_Stream0_IRQHandler`. |
| `ENCODER_SAMPLE_ISR` | `Src/Inverter/Drivers/Sensors/EncoderADC.cpp` | 429 | `HAL_ADC_ConvCpltCallback`; computes angle & speed. |
| `ISOLATED_ADC_EXTI_ISR (entry vector)` | `Src/Inverter/Drivers/Sensors/MAX22530.cpp` | 712 | `EXTI1_IRQHandler` for MAX22530. |
| `ISOLATED_ADC_EXTI_ISR (dispatch)` | `Src/Inverter/Drivers/Sensors/MAX22530.cpp` | 720 | Starts SPI DMA burst read. |
| `ISOLATED_ADC_SPI_DMA_ISR` | `Src/Inverter/Drivers/Sensors/MAX22530.cpp` | 732 | Parses isolated ADC burst and raises comparator faults. |
| `ISOLATED_ADC_SPI_DMA_ERROR_ISR` | `Src/Inverter/Drivers/Sensors/MAX22530.cpp` | 742 | SPI DMA error path. |
| `SHELL_UART_RX_ISR` | `Src/Inverter/Control/CommandShell.cpp` | 122 | `HAL_UART_RxCpltCallback`; shell byte receive. |
| `TELEMETRY_UART_TX_DMA_ISR` | `Src/Inverter/Telemetry.cpp` | 835 | `HAL_UART_TxCpltCallback`; telemetry DMA complete. |
| `SHELL_TELEMETRY_UART_ISR (entry vector)` | `Src/stm32h7xx_it.c` | 223 | `USART3_IRQHandler`. |
| `SUPPLY_MONITOR_ISR (entry vector)` | `Src/Inverter/Drivers/Logging/SupplyMonitor.cpp` | 98 | `PVD_AVD_IRQHandler`. |
| `SUPPLY_MONITOR_ISR (VDD dip)` | `Src/Inverter/Drivers/Logging/SupplyMonitor.cpp` | 106 | `HAL_PWR_PVDCallback`. |
| `SUPPLY_MONITOR_ISR (VDDA dip)` | `Src/Inverter/Drivers/Logging/SupplyMonitor.cpp` | 118 | `HAL_PWREx_AVDCallback`. |
| `FDCAN_ERROR_ISR` | `Src/Inverter/Drivers/CAN/FdcanFault.cpp` | 20 | FDCAN error-status callbacks. |
| `SYSTICK_1MS_ISR` | `Src/stm32h7xx_it.c` | 184 | `SysTick_Handler`; HAL 1 ms tick. |

### 3.3 Main-loop application domain (~100 Hz, soft real-time)

This is the primary codegen extension area for application behavior.

| Marker | File | Line | Description |
|--------|------|------|-------------|
| `APPLICATION_INIT` | `Src/Inverter/InverterMain.cpp` | 59 | `InverterMain::init()`; add app init here. |
| `APPLICATION_MAIN_LOOP` | `Src/Inverter/InverterMain.cpp` | 129 | `InverterMain::loop()` entry. |
| `HOUSEKEEPING_1HZ` | `Src/Inverter/InverterMain.cpp` | 143 | 1 Hz non-volatile / slow tasks. |
| `SENSOR_TELEMETRY_100HZ` | `Src/Inverter/InverterMain.cpp` | 154 | Phase-current / encoder telemetry poll. |
| `SAFETY_SUPERVISOR_100HZ` | `Src/Inverter/InverterMain.cpp` | 173 | Fault service, command poll, safety actions. |
| `SENSOR_TELEMETRY_100HZ` | `Src/Inverter/Drivers/Sensors/CurrentSensorTest.cpp` | 33 | `CurrentSensorTest_RunOnce()`; extend for app sensors. |
| `APPLICATION_SENSOR_POLL_100HZ` | `Src/Inverter/Drivers/Sensors/DcLinkVoltageSensor.cpp` | 80 | Example main-loop sensor poll pattern. |
| `FOC_SUPERVISOR_100HZ` | `Src/Inverter/Control/FocControlManager.cpp` | 582 | Main-loop FOC safety/startup/telemetry. |
| `OPEN_LOOP_SUPERVISOR_100HZ` | `Src/Inverter/Control/OpenLoopController.cpp` | 417 | Main-loop open-loop safety/ramp. |
| `TELEMETRY_FRAME_DISPATCH_100HZ` | `Src/Inverter/Telemetry.cpp` | 767 | `Telemetry::updateSensors()` frame sender. |
| `APPLICATION_COMMAND_REGISTRATION` | `Src/Inverter/Command/CommandInitializer.cpp` | 13 | Shell command registration table. |

### 3.4 Hardware configuration / unimplemented application ADC domain

These mark where CubeMX currently configures pins that are not yet sampled.
They are explicit codegen insertion points.

| Marker | File | Line | Description |
|--------|------|------|-------------|
| `HARDWARE_INIT` | `Src/main.c` | 98 | CubeMX peripheral init block. |
| `APPLICATION_ADC_GPIO_INIT` | `Src/adc.c` | 253 | ADC1 temp / misc analog pin init. |
| `APPLICATION_ADC_GPIO_INIT` | `Src/adc.c` | 314 | ADC2 throttle / hall analog pin init. |

## 4. Base image vs. codegen responsibilities

| Base image owns | Codegen owns |
|-----------------|--------------|
| Clock, GPIO, DMA, NVIC, TIM1/2/ADC1/ADC2/SPI2/SPI4/UART/FDCAN init | Reconfiguration of ADC sequences, CAN filters, timer periods if the app requires it |
| Fault manager, severity levels, safety actions | Raising application faults; mapping fault responses |
| Supply monitor (PVD/AVD) | — |
| Phase-current ADC ISR and overcurrent watchdog | Sensor topology changes (more current channels, different ADC arrangement) |
| Encoder DMA ISR (sin/cos analog) | Different encoder interfaces (resolver, digital ABI, etc.) |
| Isolated ADC (MAX22530) ISR/SPI/DMA | Mapping channels to sensors |
| UART shell transport and command dispatcher | Command tables and command implementations |
| Telemetry wire protocol and transport | Logged variable names and values |
| Gate-driver sequencing, PWM start/stop | Modulation strategy inside the PWM ISR |
| Safe-start / safe-stop state machines | Control law inside the control ISR |
| Example FOC + open-loop SPWM | Final chosen control + modulation combination |

## 5. Concrete codegen insertion points

### 5.1 Modulation

Replace the body of `HAL_TIM_PeriodElapsedCallback` in
`Src/Inverter/Drivers/PWM/pwm.c` with the generated modulation.  The generated
code can call existing helpers (`PWM_SetThreePhaseDuty`, `PWM_SetVoltageVector`)
or write timer registers directly.  It must respect the `foc_active` flag so the
base image can switch between open-loop and closed-loop modes.

### 5.2 Control loop

Replace `FocControlManager::onPwmPeriod()` or provide a new generated class and
point `FocControlManager_OnPwmPeriod()` at it.  Keep the safety envelope:

* Validate phase currents, DC-link voltage, encoder freshness before running math.
* Raise faults and call `requestSafeStopFromIsr()` on any dangerous condition.
* Clamp outputs to valid modulation range.

### 5.3 Application sensors

The slow analog inputs are configured in `Src/adc.c` but not sampled:

* `AIN_THROTTLE_A` (PA3 / ADC2_INP15)
* `AIN_THROTTLE_B` (PA4 / ADC2_INP18)
* `AIN_TMP_SENSE_1` (PA5 / ADC1_INP19)
* `AIN_TMP_SENSE_2` (PA1 / ADC1_INP17)
* `AIN_TMP_SENSE_3` (PA0 / ADC1_INP16)
* `AIN_MOTOR_TMP` (PF4 / ADC3_INP9)
* `AIN_HALL_W` (PA2 / ADC2_INP14)

Codegen should add either:

1. A regular-group scan on ADC2 + DMA (throttle, hall, spare channels), or
2. Software-triggered conversions in the 100 Hz main loop.

Place sampling and scaling in or near `CurrentSensorTest_RunOnce()` or create a
dedicated `ApplicationSensors` class and call it from `InverterMain::loop()`.

### 5.4 Commands

Add generated command registrations in
`Src/Inverter/Command/CommandInitializer.cpp`.  Commands are executed from main
loop context via `CommandManager::processLine()`, so they may block briefly but
must not use HAL delays if the control loop is running.

### 5.5 Telemetry

Add `Telemetry::log("<key>", value)` calls from application code.  The wire
format and DMA transport stay in the base image.

### 5.6 CAN

The base image only has FDCAN error handlers.  Codegen should add:

* CAN filter configuration in `MX_FDCANx_Init` or a generated init function.
* RX FIFO interrupt callbacks (`HAL_FDCAN_RxFifo0Callback`, etc.).
* TX scheduling for application frames.

## 6. Recommended next steps

1. **Agree on the contract** between base image and codegen:
   * Function signatures the generated code must implement.
   * Data structures it can read/write (setpoints, sensor values, fault flags).
   * ISR vs. main-loop boundaries.

2. **Create a codegen template** for the PWM ISR that outputs C code for:
   * Modulation strategy selection.
   * Control-loop wrapper.

3. **Implement one new application sensor path** end-to-end (e.g., throttle
   potentiometers) as a pilot to validate the 100 Hz application domain.

4. **Add a generated command table** replacing the hand-registered commands with
   a JSON/YAML-driven command generator.

5. **Add CAN message codegen** for commands and telemetry once the base image
   CAN RX path is added.

6. **Keep the base image buildable** after each codegen change; the existing
   FOC/SPWM examples can serve as regression tests until generated replacements
   are ready.


## 7. RTECodeEmitter integration (added after reading README.md)

The base image is now laid out for `RTECodeEmitter`.  The tool scans for single-line
markers of the form `// RTE_EMIT: <domain> <section>` and replaces them with
calls into generated `app::<DomainTitle>Init/Step` functions.

### 7.1 Domains chosen for this firmware

| Domain | Where it runs | Use case |
|--------|---------------|----------|
| `app_loop` | `InverterMain::loop()` (~100 Hz) | Throttle, temperature, CAN protocols, state machines, telemetry variables. |
| `tim_isr` | `HAL_TIM_PeriodElapsedCallback()` (TIM1 update) | PWM-synchronous modulation + control law. |
| `adc_isr` | `PhaseCurrentADC::onInjectedConversionComplete()` | PWM-synchronous current-sense processing. |

Domain names are case-insensitive to the parser but must match the NodeAPI graph
exactly.

### 7.2 Marker locations

| Marker | File | After codegen emits |
|--------|------|---------------------|
| `// RTE_EMIT: app_loop state` | `Inc/Inverter/AppState.h` | `namespace app { struct AppLoopState; }` |
| `// RTE_EMIT: tim_isr state` | `Inc/Inverter/AppState.h` | `namespace app { struct TimIsrState; }` |
| `// RTE_EMIT: adc_isr state` | `Inc/Inverter/AppState.h` | `namespace app { struct AdcIsrState; }` |
| `// RTE_EMIT: app_loop init` | `Src/Inverter/InverterMain.cpp` | `app::AppLoopInit(appState.app_loop);` |
| `// RTE_EMIT: tim_isr init` | `Src/Inverter/InverterMain.cpp` | `app::TimIsrInit(appState.tim_isr);` |
| `// RTE_EMIT: adc_isr init` | `Src/Inverter/InverterMain.cpp` | `app::AdcIsrInit(appState.adc_isr);` |
| `// RTE_EMIT: app_loop step` | `Src/Inverter/InverterMain.cpp` | `app::AppLoopStep(appState.app_loop);` |
| `// RTE_EMIT: tim_isr step` | `Src/Inverter/Drivers/PWM/pwm.cpp` | `app::TimIsrStep(appState.tim_isr);` |
| `// RTE_EMIT: adc_isr step` | `Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp` | `app::AdcIsrStep(appState.adc_isr);` |

The tool also injects `#include "<rel-path>/generated/domain_<domain>_generated.h"`
at the top of every file that contains markers.

### 7.3 New files added for the RTE contract

* `Inc/Inverter/AppState.h` — top-level state container with the three domain
  members.  Only C++ files include it.
* `Inc/Inverter/platform_api.h` — C/C++ platform API that generated code calls
  to talk to the base image (PWM, sensors, faults, time).
* `Src/Inverter/platform_api.cpp` — implementation of the platform API.  Currently
  wires existing drivers for PWM, phase currents, encoder, and Vdc; application
  sensor getters return placeholders until a sampler is implemented.

### 7.4 File changes made to support RTE

* `Src/Inverter/Drivers/PWM/pwm.c` → `pwm.cpp` so the TIM1 ISR callback is a C++
  translation unit and can include `AppState.h` / generated headers.
* `CMakeLists.txt` updated to reference `pwm.cpp` and `platform_api.cpp`.

### 7.5 How to run the emitter on this base image

```bash
RTECodeEmitter \
    --base-src  /home/aidan/Desktop/RTE/Images/Gen6FW \
    --graph     /path/to/gen6_graph.json \
    --output    /path/to/gen6_output \
    --verbosity debug
```

After running, the output tree will contain the copied firmware plus:

```
<output>/generated/
├── domain_app_loop_generated.h
├── domain_app_loop_generated.cpp
├── domain_tim_isr_generated.h
├── domain_tim_isr_generated.cpp
├── domain_adc_isr_generated.h
├── domain_adc_isr_generated.cpp
└── platform_api.h          # existing, included by generated sources
```

### 7.6 What the generated code can do

Because the generated domain source files include `platform_api.h`, node inline
code can call:

* `platform_pwm_set(du, dv, dw)` — raw duty output.
* `platform_pwm_set_voltage_vector(valpha, vbeta, vdc)` — SVPWM output.
* `platform_get_phase_currents(&iu, &iv, &iw)` — latest currents.
* `platform_get_encoder_angle(&angle_deg)` — latest encoder angle.
* `platform_get_dc_link_voltage()` — Vdc.
* `platform_get_throttle_a/b()` — application throttle inputs.
* `platform_get_motor_temperature()` / `platform_get_inverter_temperature(ch)` —
  implemented: backed by the base-image `TemperatureSensors` driver
  (TIM3 1 kHz TRGO -> ADC1 5-rank scan -> circular DMA on DMA2_Stream1,
  ADC3 continuous for the motor channel; the DMA buffer must live in
  `.dma_buffers` since DMA1/DMA2 cannot reach DTCM;
  `Hw.Temp.B1..3.*` / `Motor.Temp.*` KV config, TempSensor/Overtemperature
  faults).  Exposed to graphs via the `hw.temperatures` node template.
  The same ADC1 scan also samples the throttle inputs (PA3/PA4 are shared
  ADC1/ADC2 pins), so `platform_get_throttle_a/b()` return real volts.
* `platform_sample_application_sensors()` — trigger slow ADC sampling.
* `platform_raise_fault(source, reason)` / `platform_has_critical_fault()`.
* `platform_millis()` / `platform_micros()`.

### 7.7 Next steps specific to RTE integration

1. **Create a sample NodeAPI graph** (`gen6_graph.json`) using the three domains
   above and run `RTECodeEmitter` to verify marker replacement and compilation.
2. **Decide how `tim_isr` interacts with the base image's example FOC/SPWM code.**
   The marker is placed at the top of `HAL_TIM_PeriodElapsedCallback`; the
   example dispatch below it will still run unless the generated step returns
   early or the example code is removed.  Recommended: make the generated
   `TimIsrStep` responsible for all PWM writing, and remove/guard the example
   body once codegen is the primary path.
3. **Implement slow application ADC sampling** in `platform_sample_application_sensors()`
   or in the `app_loop` generated domain, and expose the results through the
   platform getters.
4. **Add CAN message nodes** to the graph and extend `platform_api.h` with
   `platform_can_tx()` / `platform_can_rx()` helpers once normal CAN messaging
   is added to the base image.
5. **Build the output tree** after codegen to confirm the include paths and
   renamed `pwm.cpp` compile cleanly.
