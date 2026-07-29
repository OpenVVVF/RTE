# RTE Studio

A Qt6 + QtNodes graph editor and runtime visualizer for NodeAPI graph files.

## Dependencies

- Qt6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Network)
- QtNodes — vendored as a git submodule in `third_party/QtNodes`
- NodeAPI — the RTE library that owns the graph model and JSON serialization
- InverterProtocol — optional `ivp::InverterClient` transport for live telemetry

## Build

From the RTE root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target RTEStudio -j8
```

If this is the first time building after cloning, fetch the QtNodes submodule:

```sh
git submodule update --init --recursive
```

## Run

Open a graph from the command line:

```sh
./build/bin/rte-studio Assets/Examples/foc_demo.json
```

Or launch with no arguments and use `File → Open`.

## Graph editing

- Loads node-type templates from `RTE/Assets/NodeTemplates`.
- Shows node descriptions in the palette, on canvas hover, and in the
  inspector; port and property descriptions appear as contextual tooltips.
- Parses a NodeAPI graph JSON with `NodeAPI::LoadFromJson`.
- Renders each node instance as a QtNodes node at its stored position.
- Renders `Connection`s as solid lines colored by the port's quantity.
- Renders `Bridge`s as dashed lines colored by the port's quantity.
- Draws each port as a filled shape: color = quantity, shape = frame.
- Shows an FPS / frametime overlay in the top-right corner of the viewport.
- Shows the node id, type, and timing domain in each node caption.
- Draws a colored outline around each timing domain, with the domain name labeled above it.
- Double-click a domain label or empty area inside its outline to select the
  domain, then drag its background to move every node in that domain together.
- The Runtime screen's `Export Session…` button writes a chronological JSONL
  event stream with all float and string telemetry samples, console output,
  sent console commands, connection statistics, and session metadata captured
  since the runtime started. Its final record includes per-signal sample count,
  minimum, maximum, and mean. Clearing the visible console does not discard
  export data. `Clear Session` resets the plots, latest telemetry, console,
  command history, statistics, and export archive after confirmation.
- `View → Auto Arrange` lays out the graph left-to-right by dependency flow, grouping nodes by timing domain so cross-domain bridges run between groups.
- `File → Save` / `File → Save As` writes the graph back to JSON, including any manual or auto-arranged node positions.
- `Edit → Undo` / `Redo` restores complete graph snapshots, including nodes,
  connections, bridges, positions, domains, names, and parameter edits.
- `Edit → Preferences` is the central settings window for application
  keybindings, the canvas pan mouse button, undo retention, window geometry,
  firmware build type, and build log behavior. Preferences persist between
  launches. Middle-mouse drag pans the node canvas by default, while dragging
  the empty canvas with the left button selects multiple nodes without
  selecting their connections.
- `Build → Generate Code`, `Flash`, and `Generate and Flash` operate on the
  current graph. The editor saves first, runs the firmware pipeline
  asynchronously, and streams output to the detachable Console → Logs panel.
  `F5` runs Generate and Flash.
- Interactive connection edits are validated against NodeAPI rules and persisted to JSON:
  - Producer must be an output port and consumer must be an input port.
  - Port types must match.
  - An input may have either one intra-domain connection or one bridge, not both.
  - Connections must stay within the same timing domain; bridges must cross domains.
  - Entry-point nodes cannot have incoming connections.
  - All edits are checked against the NodeAPI timing/DAG validator.
  - If a drag is rejected, the reason is shown in the status bar (bottom-left) for 4 seconds.

## Runtime / telemetry

The **Runtime** tab connects to hardware or a simulated feed and visualizes live values:

| Component | Role |
|-----------|------|
| `RuntimeController` | Bridges threaded telemetry into the Qt GUI thread |
| `TelemetryStore` | Ring-buffered float/string samples keyed by telemetry name |
| `SignalPlotWidget` | Multi-channel waveform plots with zoom and cursors |
| `SignalTablePanel` | Tabular view of latest signal values |
| `TelemetryPanel` | Summary telemetry readout |
| `ConsolePanel` | Shell command echo and device responses |
| `FlashPanel` | Firmware Update tab backed by the finite `rte flash` CLI worker |
| `LocalSessionServer` | Local automation session for external command/control |
| `LegacyTelemetryClient` | Legacy UART telemetry protocol (default) |
| `InverterProtocol` path | `Protocol::Inverter` uses `ivp::InverterClient` for new firmware |

`RuntimeController` supports `simulate=true` for UI verification without hardware.

### Connecting HostSim traces later

`Images/HostSim/` emits CSV traces and optional `platform_telemetry_log_f32` keys.
To feed NodeGUI live plots without custom glue:

1. Run HostSim with telemetry keys in the graph (`app.telemetry_log` nodes).
2. Bridge HostSim stdout/serial to the port NodeGUI opens, or extend HostSim to
   publish `InverterProtocol` frames (same path as `Protocol::Inverter`).
3. Open the Runtime tab, select the port, and choose the matching protocol.

Offline inspection: `Images/HostSim/scripts/plot_sim.py trace.csv`.

## What it does not do yet

- No packaged project/archive format or closed-loop plant simulator.
- HostSim does not yet stream `InverterProtocol` by default (CSV first).
