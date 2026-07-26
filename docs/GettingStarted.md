# Getting Started

Build and run the RTE host tools, including the **NodeGUI** node editor.

## Prerequisites

| Dependency | Notes |
|---|---|
| Git | With submodule support |
| CMake ≥ 3.24 | Host tools |
| Ninja | Recommended generator |
| C++20 compiler | GCC/Clang/MSVC; MinGW 11.2 works with Qt's toolchain |
| Qt 6 | Core, Gui, Widgets, OpenGL, OpenGLWidgets — **required for NodeGUI** |
| `arm-none-eabi-gcc` | Only needed for STM32 firmware builds |

Hardware safety docs and chassis files are in
[OpenVVVF/Hardware](https://github.com/OpenVVVF/Hardware). This repo is firmware
+ host toolchain.

## Clone

```bash
git clone --recurse-submodules https://github.com/OpenVVVF/RTE.git
cd RTE
```

If you already cloned without submodules, fetch QtNodes (used by NodeGUI):

```bash
git submodule update --init --recursive
```

The submodule lives at `Source/NodeGUI/third_party/QtNodes`
([paceholder/nodeeditor](https://github.com/paceholder/nodeeditor)).

## Configure and build (Linux / macOS)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

## Configure and build (Windows + Qt MinGW)

Example with Qt 6.7.3 and the matching MinGW from the Qt online installer /
`aqtinstall`:

```powershell
$env:Path = "C:\Qt\Tools\mingw1120_64\bin;C:\Qt\6.7.3\mingw_64\bin;" + $env:Path
$env:Qt6_DIR = "C:\Qt\6.7.3\mingw_64"
$env:MINGW_PREFIX = "C:\Qt\Tools\mingw1120_64"

# either:
cmake --preset windows-mingw-debug
cmake --build --preset nodegui-windows

# or explicit:
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="$env:Qt6_DIR" `
  -DCMAKE_C_COMPILER="$env:MINGW_PREFIX/bin/gcc.exe" `
  -DCMAKE_CXX_COMPILER="$env:MINGW_PREFIX/bin/g++.exe"
cmake --build build --target NodeGUI -j8
```

MSVC works too: install Qt's `win64_msvc*` kit, open a VS developer shell, and
pass that kit's prefix to `-DCMAKE_PREFIX_PATH`.

## What gets built

| Target | Path (typical) | Purpose |
|---|---|---|
| `NodeGUI` | `build/Source/NodeGUI/NodeGUI` | Qt node-graph editor |
| `RTECodeEmitter` | `build/Source/RTECodeEmitter/RTECodeEmitter` | Graph → patched firmware tree |
| `RTEFirmwareBuilder` | `build/Source/RTEFirmwareBuilder/RTEFirmwareBuilder` | Emit + ARM CMake build |
| `InverterCodegen` | `build/Lib/InverterCodegen/InverterCodegen` | Standalone codegen CLI |
| `*_tests` | under `build/Lib/...` | NodeAPI / protocol / emitter tests |

## Run the node editor

```bash
# Linux
./build/Source/NodeGUI/NodeGUI Assets/Examples/foc_demo.json

# Windows
.\build\Source\NodeGUI\NodeGUI.exe .\Assets\Examples\foc_demo.json
```

Or launch with no arguments and use **File → Open**.

NodeGUI loads node-type templates from `Assets/NodeTemplates` (baked in at
configure time via `RTE_NODE_TEMPLATES_DIR`).

## Run tests

```bash
ctest --test-dir build --output-on-failure
```

Protocol-only:

```bash
cmake --build build --target InverterProtocol_tests
./build/Lib/InverterProtocol/InverterProtocol_tests
```

## Next steps

1. Read [NodeGUI.md](NodeGUI.md) for editor behaviour (domains, bridges, params).
2. Read [Toolchain.md](Toolchain.md) to turn a graph into firmware.
3. Browse example graphs under `Assets/Examples/`.
