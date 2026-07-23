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
│   └── RTELogger/          # Shared logging used by the host tools
└── Source/
    ├── RTECodeEmitter/     # Inserts generated code into a base firmware tree
    └── RTEFirmwareBuilder/ # Builds the STM32 firmware from a firmware tree
```

- `Images/` contains the base firmware image that the emitter copies and modifies.
- `Lib/` contains reusable CMake libraries used by the host tools and future GUI.
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
