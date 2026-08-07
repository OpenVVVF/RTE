# RTE

RTE is an open-source model-based development toolchain for motor drives:
design control as a node graph, and the toolchain turns the graph into
flashable firmware. It ships with the base image for our OpenVVVF
STM32H723 inverter, but it is not tied to our hardware — **any platform
can be targeted by writing your own base image**: the HAL/driver layer,
plus the small `platform_api` contract the generated code calls.

This repo holds the STM32H723 base firmware image, the node-graph
libraries, the RTE Studio editor, and the tools that turn a graph into a
flashable firmware binary. A plant/inverter simulator based on
[ngspice](https://ngspice.sourceforge.io/) is planned, so graphs can be
exercised in closed loop before touching hardware.

> **Fork direction (hibyemy/RTE):** this fork prioritizes a **hybrid
> Nucleo-L476 + Tang Nano 20K FPGA** control platform (hardware FOC offload
> over SPI); simulation work is maintenance-only here. See
> [docs/ROADMAP_HYBRID_NUCLEO_FPGA.md](docs/ROADMAP_HYBRID_NUCLEO_FPGA.md).
> Cross-LLM session history lives in [agent-log/](agent-log/README.md).

Hardware designs and safety documentation live in
[OpenVVVF/Hardware](https://github.com/OpenVVVF/Hardware).

> **A note on the name:** *VVVF* stands for Variable Voltage Variable Frequency — it describes the output, not the control strategy. This platform is **not** limited to scalar V/Hz control; it supports vector control (FOC), arbitrary modulation schemes, and any control scheme you can express through the node graph.

## Layout

```
RTE/
├── Assets/
│   ├── Examples/           # Example NodeAPI graphs
│   └── NodeTemplates/      # Reusable node types for GUI + codegen
├── Images/
│   └── Gen6FW/             # STM32H7 base firmware image (HAL, startup, linker)
├── Lib/
│   ├── NodeAPI/            # Graph/node serialization and timing validation
│   ├── InverterCodegen/    # Graph -> C++ code generation engine
│   ├── RTELogger/          # Shared logging used by the host tools
│   └── InverterProtocol/   # Shared host/device telemetry + command protocol
└── Source/
    ├── NodeGUI/            # RTE Studio (Qt6 + QtNodes; source path kept stable)
    ├── RTEAutomation/      # Portable generation/build/flash/session library
    ├── RTECLI/             # Unified `rte` automation and MCP executable
    ├── RTECodeEmitter/     # Inserts generated code into a base firmware tree
    └── RTEFirmwareBuilder/ # Builds the STM32 firmware from a firmware tree
```

- `Assets/` holds graphs and node-type templates shared by NodeGUI and codegen.
- `Images/` contains the base firmware image that the emitter copies and modifies.
- `Lib/` contains reusable CMake libraries used by the host tools, GUI, and device firmware.
- `Source/` contains end-user executables.

## Porting to your platform

A base image is a normal firmware tree (HAL, startup, linker, drivers) with
three additions: `// RTE_EMIT:` markers at the timing-domain dispatch points,
an `AppState` global for the generated domain state, and an implementation
of `Images/Gen6FW/Inc/Inverter/platform_api.h` — the only contract between
generated code and hardware (PWM out, sensor reads, faults, config,
telemetry, time). `RTECodeEmitter --base-src <your-tree>` then produces a
flashable image from any graph. `Images/Gen6FW` is the reference
implementation.

## Build host tools

Requires CMake 3.24+, a C++20 compiler, Ninja, and Qt 6 (for RTE Studio).
Clone with
`--recurse-submodules` (or run `git submodule update --init --recursive`) so the
QtNodes dependency under `Source/NodeGUI/third_party` is present.

```bash
cmake -B build -G Ninja
cmake --build build -j8
```

On Linux, install the included udev rule once so non-root IDE/CLI processes can
control MCP2221A GPIO, then unplug and reconnect the adapter:

```bash
sudo install -m 0644 packaging/udev/60-rte-mcp2221.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

Host executables are always placed in `build/bin`; static libraries are placed
in `build/lib`. Firmware generated from a graph defaults to the platform user
cache, not to a nested directory in this checkout.

### RTE Studio

```bash
cmake --build build --target RTEStudio -j8
./build/bin/rte-studio Assets/Examples/foc_demo.json
```

On Windows, pass your Qt prefix to CMake (e.g. `-DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/mingw_64`).

## Test

```bash
ctest --test-dir build --output-on-failure
```

## InverterProtocol

`Lib/InverterProtocol` is a portable C/C++ library that encodes and decodes the
shared telemetry/command packet format used by the host tools and the inverter
firmware. It is split into two parts:

- `InverterProtocolCore` — HAL-free C code (constants, header, CRC16-CCITT,
  COBS, packet builders/parsers). It can be compiled into both host tools and
  bare-metal STM32 firmware.
- `InverterProtocol` — host-only C++ layer with a cross-platform serial port and
  a callback-based client (`ivp::InverterClient`).

The packet format is transport-agnostic; the UART adapter adds COBS framing and
`0x00` delimiters. Future CAN/CAN-FD adapters can reuse the same packet builders
and parsers by adding their own segmentation/reassembly.

### Protocol quick reference

- Magic: `0x544C4D31` (`TLM1`), version `1`
- Header (16 bytes, little-endian): `magic, version, msg_type, payload_len, seq, time_us`
- CRC16-CCITT (`0x1021`, init `0xFFFF`) over header + payload
- UART framing: COBS + `0x00` delimiter

Message types include the existing telemetry frames (`TELEMETRY_DATA`,
`TELEMETRY_DEFINE`) and reserved values for binary commands
(`COMMAND_REQ`, `COMMAND_RSP`, `ACK`, `NACK`).

### Running only the protocol tests

```bash
cmake --build build --target InverterProtocol_tests
./build/bin/InverterProtocol_tests
```

## Build the STM32 firmware

The unified `rte` CLI handles validation, generation, CMake configuration,
ARM toolchain detection, building, flashing, live Studio access, and MCP. The
older `RTECodeEmitter` and `RTEFirmwareBuilder` commands remain compatibility
wrappers for one release.
If you do not have `arm-none-eabi-gcc/g++` installed, run the bundled installer
first:

```bash
./Tools/install_stm32_toolchain.sh
```

Then build the baseline firmware:

```bash
./build/bin/rte build \
    --graph     Images/Gen6FW/baseline_graph.json \
    --base-source Images/Gen6FW
```

After a successful default build, the CLI prints the artifact path and writes a
manifest under the user cache (`~/.cache/rte/projects/...` on Linux,
`~/Library/Caches/RTE/...` on macOS, and Local AppData on Windows). Override
`--source-output` and `--build-dir` when a fixed workspace is required.

## Tools

- `rte-studio` — lightweight editor and owner of live device/telemetry state.
- `rte` — portable automation backend and MCP stdio server (`rte mcp`).
- `InverterCodegen` — generates C++ domain files from a NodeAPI graph JSON.
- `RTECodeEmitter` — takes a base firmware source tree and a graph, copies the
  firmware, generates domain code, and inserts it at `// RTE_EMIT:` markers.
- `RTEFirmwareBuilder` — wraps CMake, auto-detects the ARM toolchain, optionally
  runs code generation, and builds the STM32 firmware (compatibility command).

See [Automation backend](docs/automation-backend.md) for component boundaries,
the cache layout, CLI examples, MCP setup, and the external-write security gate.

## Calibration

The `cal` command runs hierarchical calibration routines; results persist to
the FRAM KV store under `Motor.*` and are consumed by graph config nodes at
boot:

- `cal list` — routine tree + stored values
- `cal all` — full profile in dependency order (poles+encoder, resistance,
  PMSM inductance/flux)
- `cal Motor.Poles`, `cal Motor.Encoder.SinCos`, `cal Motor.Resistance`,
  `cal Motor.PMSM[.Inductance|.FluxLinkage]`, `cal stop`, `cal status`

Flux linkage is measured two ways: a FOC back-EMF sweep with a joint
least-squares fit for (psi_m, V_off) — V_off captures inverter deadtime/IGBT
drop — and stores to `Motor.PMSM.FluxLinkage.Wb`, which enables the
flying-start feature (resume-into-spin with a back-EMF-matched pre-seed;
without a flux value, live starts are refused).

## Voltage sensing

`hw.phase_voltages` exposes all MAX22530 channels as graph outputs
(`V_U`, `V_V`, `V_W`, `V_Dc`, filtered reads) in the `vsense` timing domain.
Telemetered as `cg_vu_v`, `cg_vv_v`, `cg_vw_v` in the demo graph.

## Temperature, throttle, and user IO

The base-image `ApplicationSensors` driver samples all slow analog inputs
with zero CPU involvement: TIM3 TRGO at 100 Hz triggers an ADC1 regular
scan (board temps 1..3 on INP19/17/16, throttle A/B on INP15/18) into
circular DMA, and ADC3 free-runs on the motor temp channel (INP9). The CPU
never blocks on an ADC — `update()` only harvests finished conversions.

- `hw.temperatures` (motor + 3 board channels, °C; NAN when a channel is
  disabled or open/short). Per-channel KV config: `Hw.Temp.Bx.*` /
  `Motor.Temp.*` (`En`, `Type`, `R25`, `Beta`, `RSer`, `Orient`, `CritC`;
  types via `temp types`). Board channels default disabled (not populated).
  Open/short sustained 500 ms raises a Warning; over-`CritC` raises Critical.
- `hw.throttle` (dual channels normalized 0..1 via `Hw.ThrA/B.MinV/MaxV` +
  `Valid`). |A−B| > 0.10 sustained 100 ms raises Critical and zeroes both.
- `hw.digital_in` (USER_DIN_1..8) / `hw.digital_out` (USER_DOUT_1..4, pins
  5/6 = green/orange debug LEDs), pin selected by node parameter.
- Shell: `temp` (live V/ohm/°C + throttle), `temp types`, `temp reload`,
  `temp debug`. `config set/get` also reach raw KV keys (driver config).
- Rate telemetry (`hz_app_loop`, `hz_vsense`, `hz_tim_isr`, `hz_adc_isr`)
  is always on as a throughput canary.

## CAN bus

Two FDCANs (FDCAN1 "A" on PA11/PA12, FDCAN2 "B" on PB12/PB6, classic
frames; transceiver rail on PC8). Per-bus KV enables (`Can.A.En`/`Can.B.En`),
KV bit rate (`Can.BitRate`, default 500 kbit/s).

- **Graph nodes**: `hw.can_tx` (Bus/Id/Ext/Dlc/Rate + D0..D7 bytes),
  `hw.can_rx` (Bus/Id mailbox, `Fresh` on new frame). `can` shell command:
  status/send/rxdump, bus-off auto-recovery.
- **Session protocol** (KV `Can.Proto.*`, master `Can.Proto.En`): a host
  attaches (HELLO), the device answers with its info and the KV allow mask,
  the host requests capabilities (telemetry / commands / flash-reserved),
  and keeps the session alive with heartbeats (timeout drops it — fail-safe
  on host loss). Telemetry streams to the session only while granted
  (decimated, `Can.Proto.TelemDiv`); shell commands ride the same transport
  into the shared `CommandManager` only when explicitly enabled with
  `Can.Proto.AllowCmd=1` (default off while authentication is not implemented).
  Packets use the shared
  `Lib/InverterProtocol` format segmented over classic CAN frames; AUTH
  message IDs are reserved for a future password gate. IDs default
  0x700/0x701 (`Can.Proto.IdBase`).
- `Tools/can_session_client.py` — dependency-free socketcan test client
  that runs the full attach/capability/telemetry/command dance.
- Flashing over CAN: not pursued on this board. The H723 ROM FDCAN
  bootloader listens on PD0/PD1, which is not where Gen6 routes CAN; the
  planned dual-MCU hardware will let the MCUs reflash each other instead.

## Roadmap

Done recently:

- Calibration suite restored (hierarchical `cal`, results in the `Motor.*`
  KV namespace; flux via LS fit with V_off; flying start)
- Phase voltage sensing (`hw.phase_voltages`, `vsense` domain, snapshot reads)
- Implicit unit extraction (dimensionless inputs accept voltage/current;
  ToDim converters deleted)
- Config node persistence (FRAM KV store, `config set/save/list/delete`)
- PI voltage limit is now the true SVPWM linear limit (`Vdc/sqrt(3) * 0.95`)
- Temperature sensing, dual throttle with plausibility check, and user
  digital IO (`hw.temperatures`, `hw.throttle`, `hw.digital_in/out`)
- Graph-owned variables (`var.bool/var.float/var.current` + `var` shell),
  parameter-as-input, default node instance names
- Encoder/control timebase sync + angle extrapolation: killed the
  speed-dependent commutation "crackle" (encoder and FOC ran on independent
  clocks; the consumed angle stalled then caught up in speed-proportional
  steps). On-target instruments that found it are permanent: spike event
  recorder (`spikes`), encoder linearity trace (`enc_trace`), per-domain
  rate telemetry (`hz_*`)
- Bench builds default to Release — at `-O0` the CPU cannot service the
  control ISR load (`hz_app_loop` collapses to <100 Hz)

Next up, roughly in priority order:

- Current-loop tuning from measured motor parameters: run the R/L
  calibrators, compute PI gains for a target bandwidth, slew-limit the
  current references (the `control.slew` node exists, unwired)
- ngspice-based plant/inverter simulator for closed-loop graph testing
  before hardware — see [Images/HostSim/docs/Implementation_Plan.md](Images/HostSim/docs/Implementation_Plan.md)
- Sensorless (observer-based) angle path for high-speed operation
- Zip-based project format: a library that packages project assets (node
  templates as folders with `index.json` + separate `.cpp`/`.h` files, no
  inline code) into a renamed zip
- Node library expansion: CAN bus (CAN1/CAN2)
- NodeGUI: node create/delete palette, emit/flash actions, live telemetry
- Safety: compare against HARA/TARA/SWAD, verify base-image safety subsystems,
  then bring the docs in line
- Full dyno validation
