# NodeAPI

A small, generic C++20 node-graph model library for desktop. It provides typed nodes, ports, connections, a project-level node type database, per-node timing domains, optional DAG + timing-domain validation, and JSON save/load. Qt-free; depends only on nlohmann/json.

## Build

Requires CMake >= 3.24, a C++20 compiler, and Ninja.

```sh
cmake --preset linux-debug
cmake --build build/linux-debug -j8
ctest --test-dir build/linux-debug --output-on-failure
```

Release builds use `linux-release`. Windows presets can be added to `CMakePresets.json` if needed.

The optional timing-domain validator is built by default. Disable it with:

```sh
cmake --preset linux-debug -DNODEAPI_BUILD_TIMING=OFF
```

## CMake usage

After building, link the static library:

```cmake
add_subdirectory(NodeAPI)
target_link_libraries(your_app PRIVATE NodeAPI)
```

Then include the umbrella header:

```cpp
#include <NodeAPI/NodeAPI.h>
```

## API overview

### Core types

| Type | Purpose |
|------|---------|
| `NodeAPI::Graph` | Owns node types, node instances, and connections. |
| `NodeAPI::NodeType` | A reusable node template in the project's node database: ports + code pieces. |
| `NodeAPI::Node` | An instance of a `NodeType` with id, domain, and canvas position. |
| `NodeAPI::Port` | A named port with a direction and a `WireType`. |
| `NodeAPI::Connection` | Connects an output `PortRef` to an input `PortRef` within the same timing domain. |
| `NodeAPI::Bridge` | Connects an output `PortRef` in one timing domain to an input `PortRef` in another. |
| `NodeAPI::WireType` | `{quantity, frame, dtype}` tuple. |
| `NodeAPI::PortRef` | `{nodeId, portName}` endpoint reference. |

### Wire types

```cpp
using namespace NodeAPI;

WireType scalarFloat{
    .quantity = Quantity::Dimensionless,
    .frame = Frame::Scalar,
    .dtype = DType::F32
};

std::string_view unit = GetUnitLabel(Quantity::Voltage); // "V"
```

### Node types (the project database)

```cpp
NodeType valueType{
    .id = "constant.value",
    .displayName = "Value",
    .inputPorts = {},
    .outputPorts = {{"out", PortDirection::Output, scalarFloat}},
    .inlineCode = "return 0.5f;",
    .classHeader = "",
    .classDefinition = ""
};

NodeType piType{
    .id = "control.pi",
    .inputPorts = {{"error", PortDirection::Input, scalarFloat}},
    .outputPorts = {{"out", PortDirection::Output, scalarFloat}},
    .inlineCode = "return kp * error + integrator;",
    .constructorCode = "integrator = 0.0f;",
    .classHeader = "class PiController { ... };",
    .classDefinition = "float PiController::Step(float e) { ... }",
    .maxInstances = 0,   // unlimited
    .isEntryPoint = false,
    .domain = ""         // instances may choose their own domain
};

NodeType adcType{
    .id = "hw.adc.phase_currents",
    .outputPorts = {{"iu_a", PortDirection::Output, currentFloat}},
    .isEntryPoint = true,
    .domain = "isr_pwm"  // instances are forced into this domain
};

Graph graph;
graph.AddNodeType(valueType);
graph.AddNodeType(piType);
```

### Node instances and timing domains

```cpp
graph.AddNode(Node{
    .id = "throttle",
    .type = "constant.value",
    .displayName = "Throttle",
    .domain = "adc_sample",   // <-- timing domain
    .position = {10.0, 20.0},
    .parameters = {{"value", "0.5"}, {"label", "throttle"}}   // passed to constructor/init
});

graph.AddNode(Node{
    .id = "pi",
    .type = "control.pi",
    .domain = "app_loop",
    .position = {100.0, 20.0},
    .parameters = {{"kp", "1.0"}, {"ki", "0.1"}}
});
```

### Connections and type checking

```cpp
bool ok = graph.Connect(Connection{
    .id = "c1",
    .from = PortRef{.nodeId = "throttle", .portName = "out"},
    .to = PortRef{.nodeId = "pi", .portName = "error"}
});
// ok is true when endpoints exist, directions are correct, and WireTypes match.
```

### Bridges (cross-domain data flow)

Use a `Bridge` to move data from one timing domain into another. A bridge is a directed link from an output port (producer) to an input port (consumer) in a different domain:

```cpp
bool ok = graph.AddBridge(Bridge{
    .id = "adc_to_app",
    .type = scalarFloat,
    .producer = PortRef{.nodeId = "adc_value", .portName = "out"},
    .consumer = PortRef{.nodeId = "pi", .portName = "error"}
});
```

Rules enforced for bridges:

- `AddBridge` rejects missing endpoints, wrong directions, mismatched `WireType`, duplicate bridge ids, and empty bridge ids.
- The producer endpoint must be an output port; the consumer must be an input port.
- The bridge's `type` must match both the producer port type and the consumer port type.
- A consumer port may be fed by either a bridge **or** intra-domain connections, not both.

### Rules enforced by Graph

- `AddNodeType` rejects empty ids and duplicate ids.
- `AddNode` rejects empty ids, duplicate ids, and unknown `NodeType` ids. If the `NodeType` has a non-empty `domain`, the instance is forced into that domain.
- `Connect` rejects missing endpoints, wrong directions, mismatched `WireType`, duplicate connection ids, empty connection ids, and ports already fed by a bridge.
- `AddBridge` rejects missing endpoints, wrong directions, mismatched `WireType`, duplicate bridge ids, empty bridge ids, and consumers already fed by a connection or another bridge.
- `RemoveNode` deletes the node and any connections or bridges touching it.
- `TypeCheck(connection)` returns true only when both endpoints exist and their `WireType`s are equal.

### Timing-domain and DAG validation (optional)

```cpp
#include <NodeAPI/Timing.h>

NodeAPI::Timing::Validator validator;
NodeAPI::Timing::ValidationResult result = validator.Validate(graph);

if (!result.ok) {
    for (const auto& error : result.errors) {
        // "node 'x' has no timing domain assigned"
        // "connection 'c1' connects domain 'adc_sample' to domain 'app_loop'; ..."
        // "graph contains a directed cycle involving nodes: a b c"
    }
}
```

Rules:
- Every node must have a non-empty `domain`.
- Connections may only connect nodes in the same domain.
- Bridges may only connect nodes in different domains (use a `Connection` for same-domain links).
- Entry-point node types (`isEntryPoint = true`) may not have any incoming connections or bridges.
- The graph must be a DAG (no directed cycles); bridges participate in cycle detection.

`NodeType::maxInstances` limits how many instances of a type can be added to the graph (`0` = unlimited).

Cross-domain data movement is represented by a `Bridge`, not by a direct `Connection`.

### Serialization

```cpp
std::string json = NodeAPI::SaveToJson(graph);
Graph loaded = NodeAPI::LoadFromJson(json);
```

JSON format stores `name`, `nodeTypes`, `nodes`, `connections`, and `bridges`:

```json
{
  "name": "demo",
  "nodeTypes": [
    {
      "id": "control.pi",
      "displayName": "",
      "inlineCode": "return kp * error + integrator;",
      "constructorCode": "integrator = 0.0f;",
      "classHeader": "class PiController { ... };",
      "classDefinition": "float PiController::Step(float e) { ... }",
      "maxInstances": 0,
      "isEntryPoint": false,
      "inputPorts": [{"name": "error", "direction": "input", "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"}}],
      "outputPorts": [{"name": "out", "direction": "output", "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"}}]
    }
  ],
  "nodes": [
    {"id": "pi", "type": "control.pi", "displayName": "", "domain": "app_loop", "position": {"x": 0.0, "y": 0.0}, "parameters": {"kp": "1.0", "ki": "0.1"}}
  ],
  "connections": [],
  "bridges": [
    {
      "id": "adc_to_app",
      "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
      "producer": {"nodeId": "adc_value", "portName": "out"},
      "consumer": {"nodeId": "pi", "portName": "error"}
    }
  ]
}
```

### Loading node templates from disk

Projects can ship a directory of default node types and load them at startup:

```cpp
#include <NodeAPI/NodeTemplates.h>

Graph graph;
NodeAPI::LoadResult result = NodeAPI::LoadNodeTypesFromDirectory(
    graph, "/path/to/RTE/Assets/NodeTemplates");

if (!result.ok) {
    for (const auto& error : result.errors) { /* ... */ }
}
```

Each `.json` file may contain one `NodeType` object or an array of objects. The loader skips invalid files and continues, reporting errors in `result.errors`.

### String conversions

`Quantity`, `Frame`, and `DType` can be converted to/from their JSON string representations:

```cpp
std::string s = NodeAPI::ToString(Quantity::Voltage);
Quantity q = NodeAPI::QuantityFromString("voltage");
```

## Tests

Run all tests with:

```sh
ctest --test-dir build/linux-debug --output-on-failure
```

- `tests/test_nodeapi.cpp` — node types, graph operations, type checking, JSON round-trips, bridges.
- `tests/test_templates.cpp` — loading node-type templates from disk.
- `tests/test_timing.cpp` — timing-domain rules, bridges, and DAG cycle detection.

## Scope

NodeAPI intentionally does not include codegen, UI, rate-domain scheduling beyond the basic domain check, or complex validation. It gives you a graph model + type system + timing-domain safety net; the consuming project decides how to generate code or render the canvas.
