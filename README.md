# RTE

Source for the RTE motor inverter project. This repo holds the STM32 base firmware image, the node-graph toolchain libraries, and the code emitter that stitches generated control code into the firmware.

## Layout

```
RTE/
├── Images/
│   └── Gen6FW/             # STM32H7 base firmware image (HAL, startup, linker)
├── Lib/
│   ├── NodeAPI/            # Graph/node serialization and timing validation
│   └── InverterCodegen/    # Graph -> C++ code generation engine
└── Source/
    └── RTECodeEmitter/     # CLI tool that inserts generated code into a base firmware tree
```

- `Images/` contains the base firmware image that the emitter copies and modifies.
- `Lib/` contains reusable CMake libraries used by the emitter and future GUI.
- `Source/` contains end-user executables.

## Build

Requires CMake 3.24+ and a C++20 compiler.

```bash
cmake -B build -G Ninja
cmake --build build -j8
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Tools

- `InverterCodegen` — generates C++ domain files from a NodeAPI graph JSON.
- `RTECodeEmitter` — takes a base firmware source tree and a graph, copies the firmware, generates domain code, and inserts it at `// RTE_EMIT:` markers.

See `Source/RTECodeEmitter/README.md` for usage.
