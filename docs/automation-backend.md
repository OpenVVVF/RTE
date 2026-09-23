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

The MCP `rte_flash` wrapper consumes the CLI's JSONL progress stream without
putting it in the tool response. It returns one short success result or one
bounded failure message selected from the programmer diagnostics. This keeps
byte dumps and percentage updates out of an agent's context. Direct CLI and
Studio flash flows retain progress events for their local user interfaces.

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
rte device build-info
rte device signals --filter foc
rte device snapshot --bundle foc
rte device histories --signal cg_id_a --signal cg_iq_a --window-s 5
rte device control-status
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
After an app reset, `STATUS APP_WAIT=1` means the coprocessor is holding
incoming commands until it sees an application UART frame delimiter.
If the app stays silent, it releases queued commands after 1.5 seconds;
`APP_WAIT=0` means normal forwarding has resumed.
The main shell internally counts USART3 HAL errors, shell ring overflow, and
failed receive rearms. Its three telemetry log calls are currently commented
out in `InverterMain.cpp`. Control port `UART_ERRORS` and `RX_DROPPED` count
the opposite UART direction, from main MCU to coprocessor.

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

The stdio server exposes tools for project discovery, paginated graph queries and
validation, source generation, building, simulation, CAN trace recording and
export, Gen7 bridge discovery, legacy MCP2221 control, and flashing either MCU.
It also exposes main MCU mode diagnosis, device status, filtered and paginated
numeric and string telemetry, one signal, bounded numeric and string signal history, console
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

MCP responses are designed to protect model context. Telemetry defaults to 50
flat values, signal catalogs default to 50 entries, console reads default to 25
lines, single raw histories default to 200 samples, and multi-signal histories
allow no more than 400 samples total. These tools accept exact names, filters,
offsets, or limits as applicable. Source-node telemetry groups are omitted by
default because they duplicate flat values. `rte_graph_read` returns a compact
node summary and queries one graph section at a time. Node-type source code is
omitted unless `include_code` is requested with a narrow query; full graph
resources may be withheld. `rte_build_info` returns scalar identity fields and collection
counts unless `detail: full` is requested. All MCP results have a 32 KiB hard
response limit and carry a warning above 16 KiB. A side-effecting operation is
reported as completed if only its oversized details were withheld, preventing
an agent from repeating a command or flash to obtain a log.

New Gen7 main images generated by RTECodeEmitter embed a graph hash and a
manifest of effective nodes and declared telemetry signals. Firmware announces
that manifest over telemetry and publishes controller state, latched fault
names, PWM MOE, and gate readiness/fault state. `rte_build_info`,
`rte_signal_info`, and `rte_control_status` expose these fields. The catalog
uses graph port unit and description data when available; the graph format
does not encode physical ranges. Old images can still send ordinary telemetry,
but cannot prove which graph is running. `rte_device_snapshot` reads a named
FOC bundle or custom list; `rte_device_histories` reads up to eight numeric
signals under one store lock with their individual timestamps and a 400 sample
total MCP budget. The histories
are not synchronized MCU samples. `rte_device_trends` analyzes these logged
time series rather than the graph JSON. It returns sparklines; min/max/mean,
RMS and standard deviation; a time-based slope and linear-fit strength;
early-to-late mean and RMS shifts; step detection; isolated spike count;
sampling gaps; and an approximate oscillation frequency when a repeated
shape is resolved. It also calculates pairwise Pearson correlations from
shared 48-bin time buckets and reports overlap and quality. Correlation can
show signals moving together but cannot establish causation. The text result describes the pattern, while
`structuredContent.metrics` contains
the math. Compare successive calls with the same signals and window to see
how a motor adjustment changed behavior. Its 48-bin chart is auto-scaled per
signal, and periodic behavior faster than the sampled telemetry can alias;
use `rte_device_histories` to inspect individual values and timestamps. Means,
RMS values, and regression use received samples without interpolation, so
gaps can bias them; `quality: limited` flags poor window coverage or large
gaps. For larger or custom analysis, `rte_device_history_export` writes up to
eight histories to a long-form CSV (`signal,time_s,value`) inside the MCP
workspace and returns only file and statistical metadata. This lets a person
or agent use local Python and numeric libraries without placing every sample
in the model context.
Studio treats a signal as stopped reporting after two seconds without a new
sample. `rte_device_telemetry`, `rte_device_signal`, and snapshots return
`null` for its current value and keep the old measurement separately as a
last-known value. The state is `stopped_reporting` if frames still arrive,
`link_silent` if the entire link has gone quiet, or `suspended` while Studio
pauses the connection. This reports absence of data, not a measured zero or
a diagnosis of the ISR. History windows advance with elapsed time even when
no samples arrive, and trend results report the stopped state rather than
describing old values as current behavior.
New Gen7 main firmware keeps TIM1 running as a permanent measurement and
telemetry ISR after generated control stops or faults. It publishes
`control_state` for the supervisor, `control_outputs_enabled` for permission to
write generated PWM duties, and `tim_isr_running` for the ISR itself. Studio
keeps fresh manifest `tim_isr` signals live while control is `IDLE` or `FAULT`,
uses `isr_stopped` only for an explicitly stopped ISR, and uses the two-second
`stopped_reporting` fallback if an expected signal stops arriving. Native FOC
publishes `foc_running` and still marks its controller-only telemetry
`foc_stopped`. These semantics require rebuilding and reflashing the main MCU
image; the coprocessor firmware is unchanged.
Firmware build identity fields are republished every ten seconds and retain
their current value for up to 15 seconds while the link stays active.
`rte_device_signal` includes signal age and freshness; when a name is absent,
it returns a structured `not_in_build`, `configured_not_streaming`, or
`unknown_signal` code when the catalog can determine the reason.
`rte_spike_capture` reads and re-arms the firmware's existing 64-sample,
5 kHz current-spike recorder through the `spikes` inverter command; it returns
compact phase-current and angle trend analysis. Pass `include_samples: true`
only when the parsed phase current, encoder, and duty samples are needed.
Its trigger is the firmware's current threshold, set with `spikes <amps>` in
the Runtime console or through `rte_device_command`.
`rte_device_commands` sends the firmware's `help` command and parses the full
reference into a live command catalog with names, usage, descriptions,
argument ranges, and explicit motor control roles. Its response defaults to 50
commands and accepts `filter`, `offset`, and `limit`; completeness and count
verification still use the full parsed reference. Use `control start` for the
main motor control implemented by the loaded RTE graph. Use
`foc start <iq_a> [id_a]` only for the base image's native FOC diagnostic during
internal testing. These paths are mutually exclusive. It verifies the closing
line and, on newly built Gen7
firmware, the announced command count before marking the catalog complete.
Older images report `count_verified: false`. The manual equivalent is `help`
in Studio's Runtime console or `tool rte_device_commands` in its RTE command
dialog. A main MCU image rebuilt from this source labels both paths in its
help and command responses. That firmware wording requires a main MCU reflash;
the coprocessor image is unchanged.

Current Gen7 main firmware uses `fault` as its canonical fault command. Use `fault
status`, `fault sources`, `fault clear [all|warning|high|critical|source]`, or
`fault test <source>` from Studio's Runtime console or the generic device
command tools. `fault clear` defaults to all faults and resets the related
hardware latches and generated controller fault state as well as the software
mask. Gate-related resets are refused while control or PWM is active, and a
live condition that persists is reported again. `fault reset`,
`clear fault`, and legacy `clearfault` are aliases that invoke the same clear
routine.

### Firmware changes and MCP compatibility

The MCP tool names and schemas are host code; they do not regenerate from a
firmware image. Generic command sending accepts new firmware command text,
and `rte_device_commands` discovers registered commands at runtime while the
firmware's `help` output keeps its current format. Telemetry and signal
history tools accept new names from received frames; a generated Gen7 image
also supplies the signal manifest. Agents should query the connected image
again after a reflash rather than rely on a catalog from an earlier image.

Some convenience behavior is tied to specific firmware contracts: FOC plot,
snapshot, and default trend names; controller and fault status keys; the
`spikes` command and its text capture format; the `help` parser; telemetry
framing and IDs; and coprocessor bridge commands and flash settings. An image
change to any of these needs a matching host update and verification. The
root `AGENTS.md` gives the change checklist for coding agents. The existing
MCP integration test uses simulated ports and responses, so passing it does
not establish compatibility with a newly flashed image.

In Studio, the Runtime plots and signal table show the same live data and
trends; the **FOC** preset recognizes both generated `cg_` and native `foc_`
signal names. **Inspect Firmware & Signals…** shows firmware identity and
freshness. The Runtime console sends inverter commands and shows replies.
**Build → Run RTE Command…** runs the same host CLI inside Studio and shows
its output. It accepts arguments after `rte`, including `device` diagnostics,
`validate`, `generate`, `build`, `flash`, `trace`, and `mcp2221`; it does not
invoke a shell. This gives a manual route for MCP operations that lack a
dedicated button.
For an exact MCP tool route, enter `tool NAME --arguments JSON` in that dialog;
for example `tool rte_device_trends --arguments '{"signals":["cg_id_a","cg_iq_a"]}'`.
The command uses the same tool implementation and validates the same schema.
Manual inverter commands are labeled `[CLI]` in the Runtime console.

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
