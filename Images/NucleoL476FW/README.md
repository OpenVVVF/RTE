# NucleoL476FW - RTE base firmware for ST Nucleo-L476RG

Minimal RTE-compatible base image for the **Nucleo-L476RG** (STM32L476RG,
Cortex-M4F @ 80 MHz). It mirrors the `Images/Gen6FW` structure and contracts
(`STM32CubeMX` CMake target, `// RTE_EMIT:` markers, slim `platform_api`,
`generated/` glob) so the existing `RTECodeEmitter` / `RTEFirmwareBuilder`
pipeline works unchanged with `--fw-src Images/NucleoL476FW`.

## What it does out of the box

- 80 MHz SYSCLK (MSI 4 MHz + PLL), TIM1 kernel clock 80 MHz (`TIM1_CLOCK_HZ`)
- TIM1 center-aligned three-phase PWM at **10 kHz** (ARR=4000, RCR=1)
- Complementary outputs enabled with **1 us BDTR dead-time** (DTG=80 ticks)
- Static duties **U=50%, V=25%, W=75%** (distinguishable per pin on a scope)
- TIM1 update interrupt at 10 kHz hosting the RTE `tim_isr` timing domain
- LD2 (PA5) heartbeat blink at 1 Hz from the `app_loop` domain host
- SPWM open-loop ramp available in the driver (`PWM_StartSPWM`), off by default

## Pin map (all TIM1 AF1, verified STM32L476 datasheet)

| Function | Pin  | Header          |
|----------|------|-----------------|
| CH1      | PA8  | Arduino D7      |
| CH1N     | PA7  | Arduino D11     |
| CH2      | PA9  | Arduino D8      |
| CH2N     | PB0  | Arduino A3      |
| CH3      | PA10 | Arduino D2      |
| CH3N     | PB1  | morpho CN10-24  |
| BKIN     | PA6  | Arduino D12     |
| LD2      | PA5  | on-board LED    |

- **BKIN** is pinned (AF1, pull-down) but the break input is **disabled** in
  BDTR until a real gate-driver fault line is wired to it (PA6 floats on a
  bare Nucleo and would trip spuriously).
- **SPI2 stays free** for the future FPGA join link: PB13 (SCK), PB14 (MISO),
  PB15 (MOSI), PB12 (CS) are untouched by this image.

## Scope check

Probe **PA8 (D7) vs GND**: 10 kHz square wave. Duty is 50% on the plain base
image, or **60%** if the `baseline_graph.json` emitted image is flashed (the
graph overrides duties to U=60 / V=35 / W=85 every tim_isr tick, which is the
end-to-end codegen proof). PA7 (D11) shows the complementary waveform with
1 us dead-time at each edge.

## Build (Windows, tools already on this machine)

Uses the ARM GCC 13.3 bundled with STM32CubeIDE 2.0 (`C:\ST\...`), system
CMake, and any `ninja.exe` on PATH:

```powershell
powershell -File Images\NucleoL476FW\scripts\build.ps1
```

Manual equivalent:

```powershell
$gcc = "C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools\bin"
$env:PATH = "$gcc;" + $env:PATH
cmake -S Images\NucleoL476FW -B build\nucleo_fw_build -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE="$PWD\Images\NucleoL476FW\cmake\gcc-arm-none-eabi.cmake" `
    -DCMAKE_BUILD_TYPE=Debug
cmake --build build\nucleo_fw_build -j
```

Produces `STM32CubeMX.elf` / `.bin` (~18 KB flash).

> Keep build directories **outside** `Images/NucleoL476FW/` - RTECodeEmitter
> copies the entire base tree and would pick up an in-tree build dir.

On Linux, `RTEFirmwareBuilder --fw-src Images/NucleoL476FW` works as for
Gen6 (`cmake/gcc-arm-none-eabi.cmake` is auto-detected; xPack toolchain via
`Tools/install_stm32_toolchain.sh`).

## Flash (ST-Link)

The on-board ST-Link V2-1 of this board runs old firmware (V2J24) that
**STM32CubeProgrammer refuses** ("Old ST-LINK firmware"). The OpenOCD bundled
with STM32CubeIDE flashes it fine:

```powershell
powershell -File Images\NucleoL476FW\scripts\flash.ps1            # base image
powershell -File Images\NucleoL476FW\scripts\flash.ps1 -Elf <path.elf>
```

Manual equivalent (note `stlink-dap.cfg`, not `stlink.cfg`, with ST's
OpenOCD scripts):

```powershell
$oocd    = "C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.300.202509300731\tools\bin\openocd.exe"
$scripts = "C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug.openocd_2.3.200.202510310951\resources\openocd\st_scripts"
& $oocd -s $scripts -f interface/stlink-dap.cfg -f target/stm32l4x.cfg `
    -c "program build/nucleo_fw_build/STM32CubeMX.elf verify reset exit"
```

(Alternatively, upgrading the ST-Link firmware once via STM32CubeIDE's
*Help > ST-LINK Upgrade* makes `STM32_Programmer_CLI -c port=SWD` work too.)

## RTE codegen integration

Markers (same contract as Gen6):

| Marker | File |
|---|---|
| `// RTE_EMIT: app_loop state` / `tim_isr state` | `Inc/Inverter/AppState.h` |
| `// RTE_EMIT: tim_isr init` / `app_loop init` / `app_loop step` | `Src/Inverter/InverterMain.cpp` |
| `// RTE_EMIT: tim_isr step` | `Src/Inverter/Drivers/PWM/pwm.cpp` (10 kHz TIM1 update ISR) |

`Inc/Inverter/AppState.h` uses `__has_include` fallbacks so the base image
compiles standalone (pre-emit) and with any subset of generated domains.

Slim `platform_api` (`Inc/Inverter/platform_api.h`): `platform_pwm_set` and
`platform_pwm_set_voltage_vector` are wired to the TIM1 driver; all sensor
getters (`platform_get_phase_currents`, encoder, DC-link, throttle) are stubs
returning `false`/`0` until real sensing is attached; config store is
RAM-backed; telemetry is a no-op.

### Baseline graph

`baseline_graph.json` (self-contained node types, mirrors
`Assets/NodeTemplates` conventions): three `constant.duty` nodes (60/35/85)
feeding `hw.pwm.set_duty` in the `tim_isr` domain, plus a `constant.duty` ->
`app.telemetry_log` pair in `app_loop`.

Emit + rebuild + flash in one step (emitter runs in WSL Ubuntu, ARM build on
Windows):

```powershell
powershell -File Images\NucleoL476FW\scripts\emit_and_build.ps1 -Flash
```

Host tools one-time setup (WSL Ubuntu; the repo's host tools are
POSIX-oriented, and the top-level CMake needs Qt for NodeGUI, so a partial
superbuild is used):

```bash
# in WSL Ubuntu (root): g++, ninja-build via apt; cmake >= 3.24 via pip
mkdir -p /opt/rtehost && cd /opt/rtehost
ln -s /mnt/c/Users/bc200/.cursor/STMSTUFF/Lib Lib
ln -s /mnt/c/Users/bc200/.cursor/STMSTUFF/Source Source
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(RTEHost VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
add_subdirectory(Lib/NodeAPI)
add_subdirectory(Lib/RTELogger)
add_subdirectory(Lib/InverterCodegen)
add_subdirectory(Source/RTECodeEmitter)
add_subdirectory(Source/RTEFirmwareBuilder)
EOF
cmake -S . -B build -G Ninja && cmake --build build --target RTECodeEmitter -j8
```

### Verified on hardware (2026-07-26)

Emitted `baseline_graph.json` image flashed and inspected over SWD:
`TIM1 CCR1/2/3 = 2400/1400/3400` (= 60/35/85% of ARR 4000), `CCER = 0x555`
(all six outputs), `BDTR = 0x0200A850` (MOE on, OSSR, DTG=80 -> 1 us),
counter free-running - i.e. the generated `tim_isr` code is executing and
driving the PWM.

## Layout

```
Images/NucleoL476FW/
├── CMakeLists.txt              # target STM32CubeMX (Gen6-compatible shape)
├── cmake/gcc-arm-none-eabi.cmake      # M4F flags, picked up by RTEFirmwareBuilder
├── cmake/stm32cubemx/CMakeLists.txt   # HAL/CMSIS + app C sources
├── STM32L476RGTX_FLASH.ld      # 1M flash / 96K+32K RAM
├── startup_stm32l476xx.s       # ST CMSIS device startup
├── baseline_graph.json         # Phase-2 demo graph (tim_isr + app_loop)
├── Inc/                        # main/gpio/tim/it headers + hal_conf
│   └── Inverter/               # AppState.h, platform_api.h, RteParams.h, PWM driver hdr
├── Src/                        # main/gpio/tim/it/msp/system/syscalls/sysmem
│   └── Inverter/               # InverterMain.cpp, platform_api.cpp, Drivers/PWM/pwm.cpp
├── Drivers/                    # vendored CMSIS 5 + CMSIS-L4 + L4 HAL (subset of Src/)
└── scripts/                    # build.ps1, flash.ps1, emit_and_build.ps1
```

HAL/CMSIS sources vendored from STMicroelectronics GitHub
(`stm32l4xx_hal_driver`, `cmsis_device_l4`, `cmsis_core`), same as Gen6
vendors the H7 HAL.
