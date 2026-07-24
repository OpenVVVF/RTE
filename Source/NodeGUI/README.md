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
- Renders `Connection`s and `Bridge`s as QtNodes connections.
- Shows the node id, type, and timing domain in each node caption.
- `View → Auto Arrange` lays out the graph left-to-right by dependency flow, with no overlaps.
- `File → Save` / `File → Save As` writes the graph back to JSON, including any manual or auto-arranged node positions.

## What it does not do yet

- Editing is not persisted back to JSON.
- Bridges are drawn as normal connections; distinct cross-domain styling is future work.
- No parameter editing, node creation, or code generation.
