# NodeGUI — Real Time Examiner node editor

`Source/NodeGUI` is the Qt 6 + [QtNodes](https://github.com/paceholder/nodeeditor)
canvas for OpenVVVF control graphs. It edits **NodeAPI** JSON: the same format
consumed by `InverterCodegen` and `RTECodeEmitter`.

This is the in-repo replacement path for the legacy Real Time Examiner host UI:
view graphs, rearrange nodes, edit connections/bridges under NodeAPI rules, edit
parameters, and save back to JSON.

## Build / run

See [GettingStarted.md](GettingStarted.md). Short form:

```bash
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target NodeGUI -j8
./build/Source/NodeGUI/NodeGUI Assets/Examples/foc_demo.json
```

## Mental model

```
Assets/NodeTemplates/<type>/node.json   ← reusable node types (ports, params, code)
                │
                ▼
        NodeAPI::Graph (JSON)           ← instances, connections, bridges, domains
                │
                ├──► NodeGUI (edit / visualize)
                └──► InverterCodegen → C++ domains → RTECodeEmitter → firmware
```

Each **node instance** has:

- `id` — unique string in the graph
- `type` — id of a template in the type database
- `domain` — timing domain (e.g. `adc_isr`, `tim_isr`, `app_loop`)
- `position` — canvas `{x,y}` used by NodeGUI
- `parameters` — string map of knobs (gains, limits, labels, …)

Links are either:

- **Connection** — same timing domain, solid wire
- **Bridge** — different domains, dashed wire (codegen emits an inter-domain
  transfer, typically via `std::atomic`)

## UI features (current)

### File

| Action | Behaviour |
|---|---|
| Open | Load a NodeAPI graph JSON; templates come from `Assets/NodeTemplates` |
| Save / Save As | Write graph JSON including updated positions and parameters |
| Exit | Quit |

### View

| Action | Shortcut | Behaviour |
|---|---|---|
| Auto Arrange | Ctrl+Shift+A | Left-to-right layered layout by dependency; domains grouped so bridges run between groups |

### Canvas

- **Ports**: fill colour = physical quantity; shape = frame (e.g. scalar angles
  as diamonds). Matching `WireType`s are easy to spot.
- **Connections**: solid stroke, colour from quantity.
- **Bridges**: dashed stroke, colour from quantity.
- **Domain outlines**: coloured rectangle + label around each timing domain.
- **Captions**: node id / type / domain.
- **Parameter preview**: embedded read-only panel on nodes that have parameters.
- **Parameter edit**: double-click a node → modal form → values written back to
  the graph and the embedded preview refreshes.
- **FPS overlay** (optional CMake flag `NODEGUI_ENABLE_FPS_OVERLAY`): top-right
  frametime/FPS.
- **Rejected edits**: invalid connection/bridge attempts show a reason in the
  status bar (~4 s).

### Connection / bridge validation

NodeGUI delegates rules to NodeAPI (and the optional timing/DAG validator):

1. Producer must be an **output**; consumer an **input**.
2. Port `WireType`s must match (`quantity` + `frame` + `dtype`).
3. An input may have either one intra-domain connection **or** one bridge, not
   both.
4. Connections stay inside one domain; bridges must cross domains.
5. Entry-point nodes (`isEntryPoint`) reject incoming links.
6. The graph must remain a DAG across connections **and** bridges.

## Example graphs

| File | Purpose |
|---|---|
| `Assets/Examples/foc_demo.json` | Full FOC demo suitable for UI exploration |
| `Assets/Examples/foc_chain.json` | Smaller FOC chain for emitter experiments |
| `Assets/Examples/current_telemetry.json` | Telemetry-oriented graph |

Open any of these in NodeGUI after a successful build.

## What is not in NodeGUI yet

Tracked in the root roadmap / `TODO.txt`:

- Creating / deleting node instances from a palette
- Direct “Generate firmware” button (use `RTECodeEmitter` /
  `RTEFirmwareBuilder` CLIs for now)
- Live device telemetry / flashing UI (InverterProtocol library exists; host CLI
  / GUI integration still in progress)
- Zip-based project packaging

## Architecture (source map)

| File | Role |
|---|---|
| `Main.cpp` / `MainWindow.*` | Qt application shell, menus, open/save |
| `GraphScene.*` | Load/save NodeAPI graph into a QtNodes scene; layout; param dialog; domain visuals |
| `NodeGraphModel.*` | QtNodes `AbstractGraphModel` bridge with NodeAPI validation |
| `NodeDataModel.*` | Per-instance delegate + embedded parameter preview |
| `BridgeConnectionPainter.*` | Dashed bridge rendering |
| `TypedNodePainter.*` / `PortStyle.*` | Quantity/frame-aware port painting |
| `FrameRateMonitor.*` | Optional FPS overlay |

Templates directory is injected at compile time:

```cmake
RTE_NODE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/Assets/NodeTemplates"
```

## Relationship to Hardware repo docs

The Hardware [SWAD](https://github.com/OpenVVVF/Hardware/blob/main/Docs/SWAD.md)
describes the broader software architecture and refers to the RTE configuration
tool. **NodeGUI + NodeAPI + codegen in this repo** are that toolchain’s open
implementation surface. Safety artefacts (HARA/TARA) stay under Hardware `Docs/`.
