# NodeGUI

Qt 6 + QtNodes canvas for **NodeAPI** graph files — the Real Time Examiner node
editor that lives in this repo.

Full user guide: [`docs/NodeGUI.md`](../../docs/NodeGUI.md).  
Build prerequisites: [`docs/GettingStarted.md`](../../docs/GettingStarted.md).

## Dependencies

- Qt6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets)
- QtNodes — vendored as a git submodule in `third_party/QtNodes`
- NodeAPI — the RTE library that owns the graph model and JSON serialization

## Build

From the RTE root:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target NodeGUI -j8
```

On Windows, point CMake at your Qt kit, for example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.7.3\mingw_64" `
  -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1120_64/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1120_64/bin/g++.exe"
cmake --build build --target NodeGUI -j8
```

## Run

```sh
./build/Source/NodeGUI/NodeGUI Assets/Examples/foc_demo.json
```

Or launch with no arguments and use `File → Open`.

## What it does today

- Loads node-type templates from `Assets/NodeTemplates`.
- Parses a NodeAPI graph JSON with `NodeAPI::LoadFromJson`.
- Renders each node instance as a QtNodes node at its stored position.
- Renders `Connection`s as solid lines colored by the port's quantity.
- Renders `Bridge`s as dashed lines colored by the port's quantity.
- Draws each port as a filled shape: color = quantity, shape = frame.
- Shows an FPS / frametime overlay (optional `NODEGUI_ENABLE_FPS_OVERLAY`).
- Shows the node id, type, and timing domain in each node caption.
- Draws a colored outline around each timing domain.
- Embeds a read-only parameter preview on nodes that have parameters.
- Double-click a node to edit parameters; values persist through Save.
- `View → Auto Arrange` lays out by dependency flow, grouped by timing domain.
- `File → Save` / `File → Save As` writes JSON including positions + parameters.
- Interactive connection edits are validated against NodeAPI rules; rejections
  appear in the status bar for ~4 seconds.

## What it does not do yet

- No node create/delete palette.
- No in-GUI code generation / flash / live telemetry (use the CLIs +
  InverterProtocol for now).
