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

## Modulation framework contract

Modulation follows the traction-drive model: a ladder of modes (async
carrier, synchronous n-pulse, overmodulation, six-step) walked by a single
sequencer, with transitions that preserve fundamental amplitude and phase.
Three roles, strictly separated:

- **`Modulation.Sequencer`** (one per drive): owns mode selection. Inputs are
  operating-point signals (`F_Elec`, later modulation index / torque sign);
  outputs are `ModeFrom`, `ModeTo`, `Blend` (output weight of the higher
  ladder slot), and `TransActive`. Hysteresis, minimum dwell, and transition
  type/timing live only here. The ladder is data — new modes add boundary
  parameters, not new logic.
- **Modulators** (`Modulation.*`): pure per-step laws. Each takes the
  commanded voltage vector and `V_Dc` and emits `Duty_A/B/C`. Rules:
  - Carry a `ModeId` parameter: their slot in the ladder.
  - Run only when `ModeId == ModeFrom || ModeId == ModeTo`; hold 50% neutral
    otherwise. Cheap carrier schemes may run unconditionally.
  - Own continuity: on becoming `ModeTo`, produce a pattern whose
    fundamental matches the commanded vector (gain/phase-matched,
    phase-locked arm). The sequencer never blends to fix gain errors.
  - Synchronous patterns are locked to the ROTOR angle (`Theta_E` from the
    encoder). The current loop enters as magnitude `m` and a smooth phase
    offset `delta = atan2(V_Q, V_D)` computed in the dq frame. Never
    reference a synchronous pattern to atan2 of the instantaneous
    alpha/beta vector — at low |V| that angle is switching noise, and a
    nonlinear pattern turns it into voltage kicks.
  - Heavy schemes (live SHE, optimal pulse patterns) split slow/fast: an
    `app_loop` node interpolates tables (also mode-gated), a `tim_isr` node
    emits edges. Tables ship as `static const` arrays in class-based nodes.
- **`Modulation.Route3`**: routes the active triple (or blends the two
  transitioning triples for crossfade-type boundaries) and sanitizes every
  duty to finite values (50% on failure). Chain for ladders longer than two
  modes.

Transition types are per boundary: hard phase-locked switch (default; the
modulators' arm makes it thump-free), timed duty crossfade (adjacent carrier
modes only), or pattern morph (future, angle-space).
