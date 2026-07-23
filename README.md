# RTE

Source for the RTE motor inverter project. This repo holds the STM32 base firmware
image, the node-graph toolchain libraries, and the tools that turn a graph into a
flashable firmware binary.

## Layout

```
RTE/
├── Images/
│   └── Gen6FW/             # STM32H7 base firmware image (HAL, startup, linker)
├── Lib/
│   ├── NodeAPI/            # Graph/node serialization and timing validation
│   ├── InverterCodegen/    # Graph -> C++ code generation engine
│   ├── RTELogger/          # Shared logging used by the host tools
│   └── InverterProtocol/   # Shared host/device telemetry + command protocol
└── Source/
    ├── RTECodeEmitter/     # Inserts generated code into a base firmware tree
    └── RTEFirmwareBuilder/ # Builds the STM32 firmware from a firmware tree
```

- `Images/` contains the base firmware image that the emitter copies and modifies.
- `Lib/` contains reusable CMake libraries used by the host tools, future GUI, and device firmware.
- `Source/` contains end-user executables.

## Build host tools

Requires CMake 3.24+ and a C++20 compiler.

```bash
cmake -B build -G Ninja
cmake --build build -j8
```

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

- `InverterCodegen` — generates C++ domain files from a NodeAPI graph JSON.
- `RTECodeEmitter` — takes a base firmware source tree and a graph, copies the
  firmware, generates domain code, and inserts it at `// RTE_EMIT:` markers.
- `RTEFirmwareBuilder` — wraps CMake, auto-detects the ARM toolchain, optionally
  runs `RTECodeEmitter`, and builds the STM32 firmware.

See `Source/RTECodeEmitter/README.md` and `Source/RTEFirmwareBuilder/README.md`
for detailed usage.
