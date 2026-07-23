# Node Templates

This directory holds reusable `NodeAPI::NodeType` templates shipped with the RTE project. Each file is a single JSON object (or a JSON array of objects) describing one reusable node type.

The GUI and codegen tools load these automatically for new projects so users do not have to recreate common hardware and control nodes from scratch.

## File convention

- One node type per file is preferred; arrays are accepted.
- File name should use the pattern `<category>.<name>.json`, e.g. `hw.phase_currents.json`.
- Schema follows `NodeAPI::NodeType` serialization. Required fields: `id`, `inputPorts`, `outputPorts`.

## Template guidelines

- `id`: use dotted namespaces, e.g. `hw.adc.phase_currents`.
- `domain`: leave empty on the type; instances set their own domain.
- `isEntryPoint`: true only for nodes that start a timing domain (e.g. an ISR vector).
- `maxInstances`: 1 for hardware singletons, 0 for unlimited reusable math blocks.
- Code pieces are strings interpreted by the consuming codegen project.
- Inline code may reference `params.<name>` for per-instance parameters and port names for connected inputs.

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

The loader skips invalid files and continues, reporting errors instead of stopping early.
