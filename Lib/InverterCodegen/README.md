# InverterCodegen

A small C++20 code generator that turns a NodeAPI graph into embedded-friendly C++.

It links against [NodeAPI](../NodeAPI) as a library, reads a node-graph JSON file, and emits one header/source pair per timing domain. The generated code is plain C++ with no dynamic allocation, no exceptions, no STL, and no templates in the output.

## Dependencies

- CMake >= 3.24
- C++20 compiler (GCC, Clang, MSVC)
- Ninja
- [NodeAPI](../NodeAPI) (added as a subdirectory by the root project)

## Build

From the repository root:

```sh
cd /home/aidan/Desktop/InverterCodeGen
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

The executable is `build/InverterCodegen/InverterCodegen`.

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
| Input port name | `const float <name> = state.<source>.<port>;` |
| Output port name | `float& <name> = state.<node>.<port>;` |
| `instance` | The class instance (`auto& instance = state.<node>.instance;`) |
| Other identifiers | State members (function-style) or parameter locals (class-based) |

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
├── src/
│   ├── Main.cpp           # CLI entry point
│   ├── CodeGenerator.h    # Public generator API
│   └── CodeGenerator.cpp  # Domain-based code emission
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
