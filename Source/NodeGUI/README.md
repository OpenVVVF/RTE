# NodeGUI

A minimal Qt6 + QtNodes viewer for NodeAPI graph files.

## Dependencies

- Qt6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- QtNodes — vendored as a git submodule in `third_party/QtNodes`
- NodeAPI — the RTE library that owns the graph model and JSON serialization

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
./build/Source/NodeGUI/NodeGUI /home/aidan/Desktop/RTE/Assets/Examples/foc_demo.json
```

Or launch with no arguments and use `File → Open`.

## What it does today

- Loads node-type templates from `RTE/Assets/NodeTemplates`.
- Parses a NodeAPI graph JSON with `NodeAPI::LoadFromJson`.
- Renders each node instance as a QtNodes node at its stored position.
- Renders `Connection`s as solid lines colored by the port's quantity.
- Renders `Bridge`s as dashed lines colored by the port's quantity, so cross-domain links are visually distinct by linestyle.
- Draws each port as a filled shape: color = quantity, shape = frame (e.g., scalar angles are purple diamonds), so matching types are easy to spot.
- Shows an FPS / frametime overlay in the top-right corner of the viewport.
- Shows the node id in the node title.
- Shows the node type and parameter values in the node body, one value per line.
- Double-click a node to open a properties dialog for editing its parameters.
- Draws a colored outline around each timing domain, with the domain name labeled above it.
- `View → Auto Arrange` lays out the graph left-to-right by dependency flow, grouping nodes by timing domain so cross-domain bridges run between groups.
- `File → Save` / `File → Save As` writes the graph back to JSON, including any manual or auto-arranged node positions.
- Interactive connection edits are validated against NodeAPI rules and persisted to JSON:
  - Producer must be an output port and consumer must be an input port.
  - Port types must match.
  - An input may have either one intra-domain connection or one bridge, not both.
  - Connections must stay within the same timing domain; bridges must cross domains.
  - Entry-point nodes cannot have incoming connections.
  - All edits are checked against the NodeAPI timing/DAG validator.
  - If a drag is rejected, the reason is shown in the status bar (bottom-left) for 4 seconds.

## What it does not do yet

- No parameter editing, node creation, or code generation.
