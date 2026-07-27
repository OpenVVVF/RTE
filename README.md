# RTE

Source for the OpenVVVF motor inverter firmware and Real Time Examiner (RTE)
host toolchain. This repo holds the STM32H723 base firmware image, the
node-graph libraries, the Qt NodeGUI editor, and the tools that turn a graph
into a flashable firmware binary.

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
    ├── NodeGUI/            # Qt6 + QtNodes node editor
    ├── RTECodeEmitter/     # Inserts generated code into a base firmware tree
    └── RTEFirmwareBuilder/ # Builds the STM32 firmware from a firmware tree
```

- `Assets/` holds graphs and node-type templates shared by NodeGUI and codegen.
- `Images/` contains the base firmware image that the emitter copies and modifies.
- `Lib/` contains reusable CMake libraries used by the host tools, GUI, and device firmware.
- `Source/` contains end-user executables.

## Build host tools

Requires CMake 3.24+, a C++20 compiler, Ninja, and Qt 6 (for NodeGUI). Clone with
`--recurse-submodules` (or run `git submodule update --init --recursive`) so the
QtNodes dependency under `Source/NodeGUI/third_party` is present.

```bash
cmake -B build -G Ninja
cmake --build build -j8
```

### NodeGUI

```bash
cmake --build build --target NodeGUI -j8
./build/Source/NodeGUI/NodeGUI Assets/Examples/foc_demo.json
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
./build/Lib/InverterProtocol/InverterProtocol_tests
```

## Build the STM32 firmware

`RTEFirmwareBuilder` handles CMake configuration, ARM toolchain detection, and
the optional `RTECodeEmitter` step. If you do not have `arm-none-eabi-gcc/g++`
installed, run the bundled installer first:

```bash
./scripts/install_stm32_toolchain.sh
```

Then build the baseline firmware:

```bash
./build/Source/RTEFirmwareBuilder/RTEFirmwareBuilder \
    --fw-src    Images/Gen6FW \
    --build-dir build/rtetest-fw \
    --graph     Images/Gen6FW/baseline_graph.json \
    --base-src  Images/Gen6FW \
    --output    build/rtetest-fw-src \
    --verbosity info
```

After a successful build:

```
build/rtetest-fw/
├── STM32CubeMX.elf
└── STM32CubeMX.bin
```

## Tools

- `NodeGUI` — Qt node editor for NodeAPI graphs (open/save, domains, bridges,
  parameter edit, auto-arrange).
- `InverterCodegen` — generates C++ domain files from a NodeAPI graph JSON.
- `RTECodeEmitter` — takes a base firmware source tree and a graph, copies the
  firmware, generates domain code, and inserts it at `// RTE_EMIT:` markers.
- `RTEFirmwareBuilder` — wraps CMake, auto-detects the ARM toolchain, optionally
  runs `RTECodeEmitter`, and builds the STM32 firmware.

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

## Roadmap

Done recently:

- Calibration suite restored (hierarchical `cal`, results in the `Motor.*`
  KV namespace; flux via LS fit with V_off; flying start)
- Temperature sensing (`hw.temperatures` node, base-image `TemperatureSensors`
  driver on ADC1/ADC3, `Hw.Temp.*` KV config with live `config set`,
  TempSensor warnings + over-temp critical faults)
- Phase voltage sensing (`hw.phase_voltages`, `vsense` domain, snapshot reads)
- Implicit unit extraction (dimensionless inputs accept voltage/current;
  ToDim converters deleted)
- Config node persistence (FRAM KV store, `config set/save/list/delete`)
- PI voltage limit is now the true SVPWM linear limit (`Vdc/sqrt(3) * 0.95`)

Next up, roughly in priority order:

- Node parameter-as-input support in NodeAPI/codegen, so internal settings can
  be wired and programmatically controlled
- Default node instance name in templates (GUI team request)
- Graph-owned variables (latches) for modes/enables and future conditional
  logic
- Zip-based project format: a library that packages project assets (node
  templates as folders with `index.json` + separate `.cpp`/`.h` files, no
  inline code) into a renamed zip
- Node library expansion: CAN bus (CAN1/CAN2), digital inputs, digital outputs
- NodeGUI: node create/delete palette, emit/flash actions, live telemetry
- Safety: compare against HARA/TARA/SWAD, verify base-image safety subsystems,
  then bring the docs in line
- Full dyno validation
