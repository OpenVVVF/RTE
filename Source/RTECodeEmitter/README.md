# RTECodeEmitter

`RTECodeEmitter` is the integration tool that takes a NodeAPI graph and a base firmware source tree, generates the per-timing-domain C++, and inserts it into the firmware at explicit marker locations.

## Where it sits in the pipeline

```
Node graph (JSON) ──┐
                    ├──► RTECodeEmitter ──► output firmware tree ──► build + flash
Base firmware ──────┘
```

`RTECodeEmitter` does not know anything about motor control. It only knows:

- how to load and validate a NodeAPI graph,
- how to ask `InverterCodegenLib` to produce domain C++,
- how to copy a base firmware tree,
- how to find `// RTE_EMIT:` markers and replace them with the right snippet,
- how to add the required `#include` directives.

## CLI

```bash
RTECodeEmitter \
    --base-src   <base_firmware_directory> \
    --graph      <graph.json> \
    --output     <output_directory> \
    [--generated-dir <name>] \
    [--state-variable <name>] \
    [--verbosity <level>] \
    [--dry-run]
```

| Argument | Default | Description |
|---|---|---|
| `--base-src` | required | Base firmware source tree. |
| `--graph` | required | NodeAPI graph JSON file. |
| `--output` | required | Output directory. Must not overlap with `--base-src`. |
| `--generated-dir` | `generated` | Subdirectory under `--output` for generated domain files. |
| `--state-variable` | `appState` | Name of the top-level state variable used in emitted `init`/`step` calls. |
| `--verbosity` | `info` | `error`, `warning`, `info`, `debug`, `trace`. |
| `--dry-run` | off | Scan and log what would change without writing files. |

## Marker syntax

Markers are C++ single-line comments so the base firmware still compiles if the tool is not run.

```cpp
// RTE_EMIT: <domain> <section>
```

`<domain>` is the timing domain name from the graph. `<section>` is one of:

| Section | Emitted snippet |
|---|---|
| `state` | Forward declaration inside `namespace app { struct App<Domain>State; }` plus include of the generated domain header. |
| `init` | `app::App<Domain>Init(<state-var>.<domain>);` |
| `step` | `app::App<Domain>Step(<state-var>.<domain>);` |

Section names are case-insensitive.

Example markers in base firmware:

```cpp
// state.h
// RTE_EMIT: app_loop state
// RTE_EMIT: tim_isr state

// main.cpp
// RTE_EMIT: app_loop init
// RTE_EMIT: app_loop step

// isr.cpp
// RTE_EMIT: tim_isr step
```

## Generated file layout

Inside `--output`:

```
<output>/
├── <copied base firmware files>
└── <generated-dir>/
    ├── bridges_generated.h          # only if the graph has cross-domain bridges
    ├── bridges_generated.cpp
    ├── domain_app_loop_generated.h
    ├── domain_app_loop_generated.cpp
    ├── domain_tim_isr_generated.h
    ├── domain_tim_isr_generated.cpp
    └── platform_api.h
```

Each domain header declares inside `namespace app`:

```cpp
namespace app {
struct AppAppLoopState { ... };
void AppAppLoopInit(AppAppLoopState& state);
void AppAppLoopStep(AppAppLoopState& state);
} // namespace app
```

## Cross-domain bridges

Direct node connections must stay inside a single timing domain. To pass data between domains, use a `Bridge` in the graph:

```json
{
  "bridges": [
    {
      "id": "throttle_cmd",
      "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
      "producer": {"nodeId": "throttle", "portName": "out"},
      "consumer": {"nodeId": "current_ref", "portName": "in"}
    }
  ]
}
```

The codegen emits a global `std::atomic<float>` for each bridge:

```cpp
namespace app {
extern std::atomic<float> bridge_throttle_cmd;
} // namespace app
```

The producer domain writes to it after computing its output:

```cpp
bridge_throttle_cmd.store(out, std::memory_order_relaxed);
```

The consumer domain reads it as its input:

```cpp
const float in = bridge_throttle_cmd.load(std::memory_order_relaxed);
```

Bridges are type-checked by NodeAPI against the producer and consumer port types.

## What the tool does step by step

1. Validate CLI arguments (paths exist, output does not overlap base source).
2. Load the graph with `NodeAPI::LoadFromJson`.
3. Run `NodeAPI::Timing::Validator`; fail fast on timing errors.
4. Recursively copy `--base-src` to `--output`, skipping `.git`, `build`, etc.
5. Generate domain files into `output/<generated-dir>/` via `InverterCodegenLib`.
6. If the graph has bridges, generate `bridges_generated.h` / `.cpp` with global atomic variables.
7. Scan the copied source tree for `// RTE_EMIT:` markers.
8. For each marker:
   - Replace the marker line with the matching snippet.
   - Add an `#include` for the generated domain header if it is not already present.
   - Compute the relative path from the source file to `output/<generated-dir>/`.
9. Write the modified files.

## Logging

| Level | What is printed |
|---|---|
| `error` | Fatal problems. |
| `warning` | Suspect but non-fatal issues. |
| `info` | High-level progress (files generated, copy complete). |
| `debug` | Per-file copy operations and found markers. |
| `trace` | Internal parsing details. |

Errors and warnings go to `stderr`; everything else goes to `stdout`.

## Example workflow

```bash
# 1. Build the toolchain
cmake -B build -G Ninja
cmake --build build -j8

# 2. Run the emitter
./build/Source/RTECodeEmitter/RTECodeEmitter \
    --base-src  Source/RTECodeEmitter/examples/base_firmware \
    --graph     Source/RTECodeEmitter/examples/sample_graph.json \
    --output    /tmp/rtest_output \
    --verbosity debug

# 3. Compile the output
cd /tmp/rtest_output
cmake -B build -G Ninja   # if the base firmware has its own CMake
cmake --build build
```

## Future ideas (not implemented yet)

- Multi-line block markers (`// RTE_EMIT_BEGIN:` / `// RTE_EMIT_END:`) for inserting larger generated regions.
