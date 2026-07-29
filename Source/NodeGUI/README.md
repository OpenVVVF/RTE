# NodeGUI

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
cmake --build build --target NodeGUI -j8
```

If this is the first time building after cloning, fetch the QtNodes submodule:

```sh
git submodule update --init --recursive
```

## Run

Open a graph from the command line:

```sh
./build/Source/NodeGUI/NodeGUI Assets/Examples/foc_demo.json
```

Or launch with no arguments and use `File → Open`.

## Graph editing

- Loads node-type templates from `Assets/NodeTemplates`.
- Parses a NodeAPI graph JSON with `NodeAPI::LoadFromJson`.
- Renders each node instance as a QtNodes node at its stored position.
- Renders `Connection`s as solid lines colored by the port's quantity.
- Renders `Bridge`s as dashed lines colored by the port's quantity.
- Draws each port as a filled shape: color = quantity, shape = frame.
- Shows an FPS / frametime overlay in the top-right corner of the viewport.
- Shows the node id, type, and timing domain in each node caption.
- Draws a colored outline around each timing domain.
- `View → Auto Arrange` lays out the graph left-to-right by dependency flow.
- `File → Save` / `File → Save As` writes the graph back to JSON.
- Interactive connection edits are validated against NodeAPI rules.
- **Inspector panel** — edit node parameters in place.
- **Node palette** — add new node instances from templates.
- **Parameter blocks** — inline parameter editing on the canvas.

## Runtime / telemetry (merged from upstream)

The **Runtime** tab connects to hardware or a simulated feed and visualizes live values:

| Component | Role |
|-----------|------|
| `RuntimeController` | Bridges threaded telemetry into the Qt GUI thread |
| `TelemetryStore` | Ring-buffered float/string samples keyed by telemetry name |
| `SignalPlotWidget` | Multi-channel waveform plots with zoom and cursors |
| `SignalTablePanel` | Tabular view of latest signal values |
| `TelemetryPanel` | Summary telemetry readout |
| `ConsolePanel` | Shell command echo and device responses |
| `FlashPanel` / `FirmwareUpdater` | Suspend telemetry, flash firmware, resume |
| `HttpApiServer` | Local HTTP API for remote command/control |
| `LegacyTelemetryClient` | Legacy UART telemetry protocol (default) |
| `InverterProtocol` path | `Protocol::Inverter` uses `ivp::InverterClient` for new firmware |

`RuntimeController` supports `simulate=true` for UI verification without hardware.

### Connecting HostSim live (Path A)

1. Build and run HostSim in live mode:
   ```powershell
   Images\HostSim\build\Debug\host_sim.exe Images\HostSim\scenarios\default_motor.json --live
   ```
2. Launch NodeGUI against the TCP publisher:
   ```powershell
   NodeGUI --tcp 127.0.0.1:14608 --protocol ivp
   ```
3. Open the **Runtime** tab — plots receive `throttle_*`, `duty_*`, `i_*`,
   `theta_e`, `omega_e`, `vdc_v`.
4. Use the console to adjust: `throttle a 0.5`, `clear`, `quit`.

Offline inspection: `Images/HostSim/scripts/plot_sim.py trace.csv`.

## What it does not do yet

- No code generation from the GUI (use `RTECodeEmitter` separately).
- HostSim live mode uses TCP + InverterProtocol (not the legacy UART framing).
