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
* The application layer (throttle mapping, temperature monitoring, CAN protocols,
  state machines, command tables, telemetry variables).

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
