# RTE (Real-Time Embedded) Toolchain

This is the monorepo for the RTE toolchain: a node-graph based code generator for motor inverter firmware, plus future GUI, compiler integration, and simulator work.

## Layout

```
RTE/
├── Lib/
│   ├── NodeAPI/          # Graph/node serialization and timing validation
│   └── InverterCodegen/  # Graph -> C++ code generation engine
└── Source/
    └── RTECodeEmitter/   # CLI tool that inserts generated code into a base firmware tree
```

- `Lib/` contains reusable CMake libraries.
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
