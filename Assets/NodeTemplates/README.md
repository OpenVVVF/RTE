# Node Templates

This directory holds reusable `NodeAPI::NodeType` templates shipped with the RTE project. Each node type lives in its own folder so the metadata, port definitions, and C++ code blocks are easy to read and edit by hand.

The GUI and codegen tools load these automatically for new projects so users do not have to recreate common hardware and control nodes from scratch.

## Folder convention

Each node type is a directory named `<category>.<name>/` containing:

| File | Purpose |
|------|---------|
| `node.json` | Metadata: `id`, names and descriptions, ports, properties, instance limits, and timing-domain behavior. |
| `inline.cpp` | Per-step code body. Runs once per timing-domain invocation. |
| `constructor.cpp` | Optional constructor / init body for class-based nodes. |
| `class_header.h` | Optional C++ class declaration. |
| `class_definition.cpp` | Optional C++ class implementation. |

Only `node.json` is required. Code-block files are read only if they exist.

Example layout:

```
Assets/NodeTemplates/
  control.pi/
    node.json
    inline.cpp
  hw.phase_currents/
    node.json
    inline.cpp
```

## Schema notes

- `id`: use dotted namespaces, e.g. `hw.adc.phase_currents`.
- `description`: a concise explanation of what the node does. The editor shows
  it in the node palette, on canvas hover, and in the inspector.
- `domain`: leave empty on the type; instances set their own domain, unless the type forces one.
- `isEntryPoint`: true only for nodes that start a timing domain (e.g. an ISR vector).
- `maxInstances`: 1 for hardware singletons, 0 for unlimited reusable math blocks.
- `parameterTypes`: map of property name to `WireType`. Each property type also
  carries a `description` shown in the inspector. The description is UI
  metadata and does not affect code generation. Parameters not listed default
  to dimensionless `float`.
- Every input and output port has a `description` alongside its name,
  direction, and type. Hovering its connection point in the canvas shows the
  description.
- Port and property types use `{quantity, frame, dtype}`. Physical quantities
  use the Au-backed types emitted by `InverterCodegen` (see
  `Lib/InverterCodegen/include/InverterCodegen/RteQuantity.h`).

## Code block conventions

- Reference input/output ports by their declared names.
- Reference parameters by their plain names (e.g. `kp`, not `params.kp`).
- Physical ports have Au `Quantity` types; use `.in(au::<unit>)` when you need a raw `float` for math or HAL calls.
- All ports are scalar (`"frame": "scalar"`); combined frames are expressed as separate scalar ports (e.g. `ia`/`ib`/`ic`, `v_alpha`/`v_beta`).

## Loading templates

Call from C++:

```cpp
#include <NodeAPI/NodeTemplates.h>

NodeAPI::Graph graph;
NodeAPI::LoadResult result = NodeAPI::LoadNodeTypesFromDirectory(graph, "/path/to/NodeTemplates");
if (!result.ok) {
    // result.errors holds human-readable messages, result.filesLoaded holds the count.
}
```

The loader skips invalid folders and continues, reporting errors instead of stopping early.
