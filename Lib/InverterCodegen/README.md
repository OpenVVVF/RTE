# InverterCodegen

A small C++20 code generator that turns a NodeAPI graph into embedded-friendly C++.

It links against [NodeAPI](../NodeAPI) as a library, reads a node-graph JSON file, and emits one header/source pair per timing domain. The generated code is plain C++ with no dynamic allocation, no exceptions, and no templates in the output.

Physical quantities use the [Au](https://github.com/aurora-opensource/au) units library (vendored as a single header). Currents, voltages, torques, temperatures, and angular velocities are compile-time unit-checked; dimensionless values and booleans stay as plain `float`/`bool`.

## Dependencies

- CMake >= 3.24
- C++20 compiler (GCC, Clang, MSVC)
- Ninja
- [NodeAPI](../NodeAPI) (added as a subdirectory by the root project)

## Build

From the repository root:

```sh
cd /home/aidan/Desktop/RTE
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

The executable is `build/Lib/InverterCodegen/InverterCodegen`.

## How it works

The generator consumes a NodeAPI graph. Each node instance belongs to a timing domain (`domain` field). Nodes in the same domain are topologically sorted and emitted together:

```
domain_<name>_generated.h
  - class declarations from NodeType::classHeader
  - App<Name>State struct
domain_<name>_generated.cpp
  - class definitions from NodeType::classDefinition
  - App<Name>Init(state)
  - App<Name>Step(state)
```

### Codegen convention

Inside `NodeType::inlineCode` and `NodeType::constructorCode`, variable names are resolved as follows:

| Name | Maps to |
|---|---|
| Input port name | `const <type> <name> = state.<source>.<port>;` |
| Output port name | `<type>& <name> = state.<node>.<port>;` |
| `instance` | The class instance (`auto& instance = state.<node>.instance;`) |
| Other identifiers | State members (function-style) or parameter locals (class-based) |

`<type>` is chosen from `InverterCodegen/RteQuantity.h` based on the port's `quantity` and `frame`.

#### Parameter types

`NodeType::parameterTypes` maps a parameter name to a `WireType`. The generator uses this to emit the right unit constructor, e.g.:

```json
"parameterTypes": {
  "kp": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
  "limit": {"quantity": "current", "frame": "scalar", "dtype": "f32"}
}
```

emits `state.node.kp = 0.5f;` and `state.node.limit = rte::Amperes(10.0f);`.

#### Function-style node

```json
{
  "id": "control.gain",
  "inputPorts": [{"name": "in", ...}],
  "outputPorts": [{"name": "out", ...}],
  "inlineCode": "out = gain * in;"
}
```

`gain` is not a port, so it becomes a state member initialized from `Node.parameters["gain"]`.

#### Class-based node

```json
{
  "id": "control.pi",
  "inputPorts": [{"name": "error", ...}],
  "outputPorts": [{"name": "out", ...}],
  "inlineCode": "out = instance.update(error);",
  "constructorCode": "instance.kp = kp; instance.ki = ki;",
  "classHeader": "class PiController { public: float kp; float ki; float integrator; float update(float error); };",
  "classDefinition": "float PiController::update(float error) { ... }"
}
```

`instance` is the class object; `kp` and `ki` are parameter locals available in `constructorCode` and `inlineCode` when used.

The generated source includes `"platform_api.h"`, which the base firmware provides for functions such as `platform_pwm_set`.

## Usage

```sh
./build/InverterCodegen/InverterCodegen <graph.json> <output_dir>
```

Example:

```sh
./build/InverterCodegen/InverterCodegen \
    InverterCodegen/examples/sample_graph.json \
    InverterCodegen/generated
```

Before generating, the tool runs `NodeAPI::Timing::Validator` on the graph. It fails if nodes are missing domains, connections cross domains, or the graph contains a cycle.

## Project layout

```
InverterCodegen/
├── CMakeLists.txt
├── README.md
├── include/InverterCodegen/
│   ├── CodeGenerator.h    # Public generator API
│   └── RteQuantity.h      # Au-backed quantity aliases
├── src/
│   ├── Main.cpp           # CLI entry point
│   └── CodeGenerator.cpp  # Domain-based code emission
├── third_party/
│   └── au/au.hh           # Vendored Au units library
├── examples/
│   ├── sample_graph.json          # Function-style example
│   └── sample_graph_class.json    # Class-based PI example
└── generated/             # Example output (not committed)
```

## Scope

InverterCodegen intentionally does not:

- Scan or modify base firmware source files.
- Resolve header dependencies across a project.
- Insert generated code into an existing code base.

Those responsibilities live in a separate integration tool that consumes this library.
