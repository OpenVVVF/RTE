# Automation backend

RTE has one automation surface: the portable `rte` executable. RTE Studio is a
thin interactive frontend and the owner of state that must stay alive (the open
graph, serial connection, telemetry history, and device console). Generation,
validation, firmware builds, and flashing are finite CLI jobs.

## Boundaries

- `RTEAutomation` contains reusable, Qt-free generation, CMake, process,
  flashing, cache, and Studio-session code.
- `rte` exposes that library to terminals, scripts, CI, RTE Studio, and MCP.
- `rte-studio` edits graphs and owns the live device session. It launches `rte`
  with argument arrays and consumes JSON Lines events; it does not invoke a
  shell or host the build system.
- `RTECodeEmitter` and `RTEFirmwareBuilder` are compatibility wrappers. New
  automation should use `rte`.

All host executables land in `build/bin`, with libraries in `build/lib`. A
default firmware build uses a stable project ID derived from the graph path:

```text
<user-cache>/rte/projects/<project-id>/
  generated/
  build/<configuration>/
  artifacts/
  manifest.json
```

That makes the old `build/Source/...` host-binary layout and ad-hoc generated
source directories irrelevant to normal workflows. Explicit `--source-output`
and `--build-dir` options remain available for reproducible CI workspaces.

## CLI

```bash
rte validate --graph graph.json --templates Assets/NodeTemplates
rte generate --graph graph.json --base-source Images/Gen6FW --output out
rte build --graph graph.json --base-source Images/Gen6FW
rte flash --firmware firmware.bin --serial /dev/ttyACM0
```

Use `--format json` for one structured result or `--format jsonl` for progress
events. Commands never require a local web server.

`rte flash` controls MCP2221A GP0 (BOOT0) and GP1 (active-low NRST) directly
through USB HID before and after invoking STM32CubeProgrammer. This native path
is the default and does not require Python or EasyMCP2221. Pass
`--manual-boot` only when BOOT0 and reset will be controlled by hand; automatic
mode reports a hard error when the MCP2221A is missing or inaccessible instead
of silently continuing in manual mode.

When RTE Studio is running, read its device state through the discovered local
session:

```bash
rte device status
rte device telemetry
rte device console --since 0 --lines 100
rte device command status
```

The session is bound to `127.0.0.1`, uses a random token stored in the user-only
cache descriptor, and disappears when Studio exits. Device commands and a
Studio-coordinated flash are refused unless **Preferences → Automation → Allow
CLI and MCP clients to send commands to the device** is enabled. Read-only
status, telemetry, and console access remains available.

## MCP

Configure an MCP client to launch:

```text
rte mcp --workspace /path/to/project
```

The server uses JSON-RPC over stdin/stdout and exposes project tools
(`rte_validate`, `rte_generate`, `rte_build`, `rte_flash`) plus live device
tools (`rte_device_status`, `rte_device_telemetry`, `rte_device_console`, and
the gated `rte_device_command`). MCP is an adapter over the same CLI/library
boundary, not a second backend.

## Distribution

`cmake --install build --prefix stage` creates the portable layout and deploys
the required Qt runtime. `.github/workflows/build-artifacts.yml` builds, tests,
and uploads unsigned x64 archives for Linux, Windows, and macOS.
