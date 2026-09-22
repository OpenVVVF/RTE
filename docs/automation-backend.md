# Automation backend

RTE has one automation surface: the portable `rte` executable. RTE Studio is a
thin interactive frontend and the owner of state that must stay alive (the open
graph, serial connection, telemetry history, and device console). Generation,
validation, firmware builds, and flashing are finite CLI jobs.

## Boundaries

- `RTEAutomation` contains reusable, Qt-free generation, CMake, process,
  flashing, cache, and Studio-session code.
- `rte` exposes that library to terminals, scripts, CI, RTE Studio, and MCP.
- `RTEStudio` edits graphs and owns the live device session. It launches `rte`
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
rte device mode
rte device mode --probe-bootloader
rte sim --graph graph.json [--scenario file.json] [--base-source DIR] [--name NAME]
        [--live] [--realtime F] [--no-build] [--output-format text|json|jsonl]
```

Use `--format json` for one structured result or `--format jsonl` for progress
events. Commands never require a local web server.

`rte sim` emits a graph into the HostSim base image (default `Images/HostSim`
in the same checkout, discovered by walking up from the `rte` executable),
builds it with cmake under `build/hostsim_<name>_emitted_build`, and runs
`host_sim` in the foreground. `<name>` defaults to the graph file stem and is
restricted to `[A-Za-z0-9_.-]` (no separators, not `.`/`..`) because it
composes build directories that are wiped between emits. When `rte` runs
outside a source checkout (an installed binary finds no repo root), the sim
workspace moves to the user cache (`~/.cache/rte/sim/` on Linux) instead of
the install prefix.

**Current status:** Simulation is not correctly implemented and is not advised
for control tuning, firmware validation, or hardware decisions. `rte sim`
remains available for simulator development and emits a warning when run.

- `RTE_EMITTER` overrides the RTECodeEmitter executable path; a set-but-missing
  value falls through to the emitter next to `rte` (or on `PATH`), and the
  final error names the bad path.
- Without `--scenario`, the scenario matching the effective name — `--name` if
  given, else the graph stem — minus a trailing `_graph` under
  `<base-source>/scenarios/` is used, falling back to
  `scenarios/default_motor.json` — the same rule as
  `Images/HostSim/scripts/run_spwm_live.sh`.
- Batch mode defaults to `--realtime 0` (as fast as the host can run); with
  `--live` the default is 1.0 (wall-clock). Live mode reports the IVP telemetry
  endpoint in the progress message/structured event — the scenario's
  `simulation.listen_host`/`listen_port` when set (passed to `host_sim` as
  `--listen`, because a bare `--live` there would otherwise pin its CLI
  defaults), else the default `127.0.0.1:14608` — and stays in the foreground
  until Ctrl+C.
- POSIX: SIGINT/SIGTERM/SIGHUP delivered to `rte` are forwarded to the running
  sim child before exit, so an interrupted run never orphans `host_sim`.
  Windows: `host_sim` is created with `CREATE_NO_WINDOW`, so Ctrl+C in a
  console does not reach it and closing the terminal can leave it running —
  stop it with Task Manager or `Stop-Process -Name host_sim`.
- The simulator's trace CSV is a batch-mode artifact (live runs write none);
  it lands in the run directory
  `build/hostsim_<name>_emitted_build/run/`; the completed run reports the
  absolute path as a `sim-trace` artifact event. `--no-build` reuses the most
  recent emit and/or build for the name.

`rte flash --firmware main.elf` flashes the Gen7 main processor. On Linux it
discovers the matching OpenVVVF USB CDC ports under `/dev/serial/by-id`: `if00`
for the UART bridge and `if02` for coprocessor control. It sends `BOOTLOADER`
on `if02` at 115200 8N1, programs and verifies over `if00` at 460800 8E1,
then sends `APP` on `if02`. It retries the programmer up to three times. Pass
`--serial` and `--control-port` to select ports explicitly, `--attempts 1..10`
to change retries, or `--manual-boot` when the main MCU is already in its ROM
bootloader. `--programmer` selects `STM32_Programmer_CLI` explicitly.

`rte flash --target coproc --firmware coproc.elf` flashes the coprocessor
through STM32 USB DFU (`port=usb1`) and starts the ELF entry point afterward.
The coprocessor must already be in DFU mode. A `.bin` image uses flash address
`0x08000000` and its reset vector for the start address. For a `.hex` image,
start the application separately after flashing because the start address is
not derived from the HEX file. These sequences follow the scripts in
`Images/Gen7FW/MainProcessor` and `Images/Gen7FW/CoProcessor`.

The legacy `rte mcp2221 enter|exit|release` command remains available for
Gen6 hardware diagnosis; it is no longer used by `rte flash`.

When RTE Studio is running, read its device state through the discovered local
session:

```bash
rte device status
rte device telemetry
rte device console --since 0 --lines 100
rte device command status
rte device mode
```

`rte device mode` is a read only Gen7 diagnosis that does not modify either
firmware image. It asks the coprocessor control port (`if02`) for `STATUS` and
checks for fresh valid application frames on the UART bridge (`if00`). When
RTE Studio owns that bridge, it checks Studio's frame counter instead of
opening the port a second time. The MCP equivalent is `rte_device_mode`.

The result uses these states:

| State | Meaning |
|---|---|
| `app_responding` | Valid application frames arrived during the sample. |
| `bootloader_selected` | Coprocessor selected the main MCU boot UART mode; MCU response is unverified. |
| `bootloader_responding` | Optional connect only STM32CubeProgrammer probe reached the ROM bootloader. |
| `bootloader_unresponsive` | That probe failed; this can also be a UART or power problem. |
| `app_unresponsive_or_silent` | App UART mode is selected but no valid frames arrived. A hung MCU cannot be distinguished from a silent app, no power, or a broken link. |
| `unknown` | Available signals do not establish a mode. |

Pass `--probe-bootloader` (MCP: `probe_bootloader: true`) to run the
[STM32CubeProgrammer connect command](https://dev.st.com/stm32cube-docs/prog/2.23.0/en/docs/markup/Uart_Connection_page.html)
without download or flash options. The probe is skipped when Studio owns the
bridge. `--serial`, `--control-port`, and `--programmer` override discovery.
The coprocessor's `STATUS` reports its own selected UART mode, not an
independent main MCU heartbeat, so an unresponsive app is a diagnosis lead
rather than proof of a CPU hang.

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

The stdio server exposes tools for project discovery, graph reading and
validation, source generation, building, simulation, CAN trace recording and
export, Gen7 bridge discovery, legacy MCP2221 control, and flashing either MCU.
It also exposes main MCU mode diagnosis, device status, all latest numeric and string telemetry, one
signal, numeric and string signal history, console
lines, unrestricted command text, and a command-and-response time window.
The command response tool returns console lines received after the command;
the inverter protocol does not tag console replies with request IDs, so
unrelated console output may appear in that window. The `console_since` cursor
from `rte_device_command` allows later reads with `rte_device_console`.
RTE Studio's Runtime console shows MCP tool actions, write outcomes, and read
failures as `[MCP]` lines. Inverter commands appear as `[MCP] > <command>` or `[CLI] > <command>`,
followed by a send result. Frequent reads are logged at most once per five
seconds per tool. These audit lines are included in session exports; the
command-response tool ignores them when deciding if the inverter replied.
The server also lists workspace graph and README resources and offers an
inverter diagnostics prompt. It uses the same Studio session and flash worker
as the CLI. Studio must be running and connected for live device tools.

For Codex CLI or the ChatGPT desktop app, add this to `~/.codex/config.toml`
(or a trusted project's `.codex/config.toml`; see the
[official OpenAI MCP documentation](https://learn.chatgpt.com/docs/extend/mcp)):

```toml
[mcp_servers.rte]
command = "/absolute/path/to/RTE/build/bin/rte"
args = ["mcp", "--workspace", "/absolute/path/to/RTE"]
tool_timeout_sec = 600
```

For Kimi Code, add this to `~/.kimi-code/mcp.json` or a trusted project's
`.kimi-code/mcp.json` (see the
[Kimi Code MCP documentation](https://www.kimi.com/code/docs/en/kimi-code-cli/customization/mcp.html)):

```json
{
  "mcpServers": {
    "rte": {
      "command": "/absolute/path/to/RTE/build/bin/rte",
      "args": ["mcp", "--workspace", "/absolute/path/to/RTE"],
      "toolTimeoutMs": 600000
    }
  }
}
```

The ChatGPT web app cannot launch a local stdio process; it needs a separately
hosted MCP connection or a supported tunnel. The local setup above is for
Codex, ChatGPT desktop, and Kimi Code on the machine connected to the inverter.

## Distribution

`cmake --install build --prefix stage` creates the portable layout and deploys
the required Qt runtime. `.github/workflows/build-artifacts.yml` builds, tests,
and uploads unsigned x64 archives for Linux, Windows, and macOS.
