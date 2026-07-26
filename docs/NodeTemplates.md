# Node Templates

Reusable node types live under `Assets/NodeTemplates/`. NodeGUI, codegen, and
the emitter all load them through `NodeAPI::LoadNodeTypesFromDirectory`.

## Layout

```
Assets/NodeTemplates/
├── control.pi/
│   ├── node.json          # required: ports, parameters, metadata
│   ├── node.cpp           # optional: class / inline implementations
│   └── node.h             # optional: declarations referenced by codegen
├── math.clarke/
│   └── ...
└── hw.pwm.set_duty/
    └── ...
```

Each directory name matches the template `id` (e.g. `control.pi`).

## `node.json` fields

Minimal example (`control.pi` abbreviated):

```json
{
  "id": "control.pi",
  "displayName": "PI Controller",
  "maxInstances": 0,
  "isEntryPoint": false,
  "inputPorts": [
    {
      "name": "Setpoint",
      "direction": "input",
      "type": { "quantity": "dimensionless", "frame": "scalar", "dtype": "f32" }
    }
  ],
  "outputPorts": [
    {
      "name": "Output",
      "direction": "output",
      "type": { "quantity": "dimensionless", "frame": "scalar", "dtype": "f32" }
    }
  ],
  "parameterTypes": {
    "Kp": { "quantity": "dimensionless", "frame": "scalar", "dtype": "f32" },
    "Ki": { "quantity": "dimensionless", "frame": "scalar", "dtype": "f32" }
  }
}
```

| Field | Meaning |
|---|---|
| `id` | Stable type key referenced by node instances |
| `displayName` | UI label |
| `maxInstances` | `0` = unlimited |
| `isEntryPoint` | If true, rejects incoming connections/bridges |
| `domain` | Optional forced domain for every instance |
| `inputPorts` / `outputPorts` | Named ports with `WireType` |
| `parameterTypes` | Types for instance `parameters` (drives units in codegen + NodeGUI preview) |
| `inlineCode` / `constructorCode` / `classHeader` / `classDefinition` | Codegen fragments (may also live in companion `.cpp`/`.h`) |

### WireType

```json
{ "quantity": "current", "frame": "abc", "dtype": "f32" }
```

- **quantity** — physical meaning (`current`, `voltage`, `angle`, …); colours ports in NodeGUI
- **frame** — representation (`scalar`, `abc`, `dq`, …); shapes ports in NodeGUI
- **dtype** — storage (`f32`, `bool`, …)

## Shipping instances in a graph

Graphs reference types by id and supply per-instance parameters:

```json
{
  "id": "IqPi",
  "type": "control.pi",
  "domain": "tim_isr",
  "position": { "x": 420.0, "y": 180.0 },
  "parameters": {
    "Kp": "0.5",
    "Ki": "200.0",
    "OutputMax": "1.0",
    "OutputMin": "-1.0"
  }
}
```

NodeGUI shows these parameters on the node body and lets you edit them via
double-click.

## Adding a new template

1. Create `Assets/NodeTemplates/<id>/node.json` with ports + parameterTypes.
2. Add codegen fragments (inline or class-based) per
   `Lib/InverterCodegen/README.md`.
3. Rebuild host tools (templates are loaded at **runtime** from disk for NodeGUI
   via `RTE_NODE_TEMPLATES_DIR`, so a NodeGUI rebuild is only needed if the
   path macro / binary changes).
4. Drop an instance into an example graph or create one in JSON.
5. Open in NodeGUI, Auto Arrange, verify port colours/shapes, emit with
   `RTECodeEmitter`.

## Current library (snapshot)

| Prefix | Examples |
|---|---|
| `hw.*` | phase currents, DC link voltage, encoder, PWM duty |
| `math.*` | Clarke/Park, SVPWM, sincos, unit conversions |
| `control.*` | PI, current PI |
| `constant.*` / `config.*` | constants and config values |
| `app.*` | telemetry log / sink |

Exact set: list directories under `Assets/NodeTemplates/`.
