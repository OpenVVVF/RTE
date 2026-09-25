# RTE MCP: setup instructions for an AI agent

Use this file when asked to set up the RTE MCP server for Codex, ChatGPT
desktop, or Kimi Code. Work from this repository's root. **Build the current
source before configuring a client or starting the server.** Do not use an
older `rte` binary found on `PATH`.

If changing firmware as well as using the MCP, read `AGENTS.md`. It lists the
firmware interfaces that require matching MCP, Studio, or protocol changes.
The tool list alone cannot guarantee compatibility with a newly changed image.

## Updating an existing RTE MCP installation

Use this path if an earlier RTE MCP already works on this machine:

1. Find the existing `rte` MCP entry in the client configuration and note its
   `command` and `--workspace` paths. Keep the entry; update it in place so
   the client does not start two RTE servers.
2. Bring the repository checkout up to date. For a clean checkout tracking
   its intended branch, run `git pull --ff-only`. If there are local changes
   or the branch has diverged, preserve and integrate them before building.
3. Run the full build and test commands in section 1 below. Both `rte` and
   `RTEStudio` must come from the updated source: the MCP and Studio session
   endpoint evolve together. If the MCP entry points to an installed copy,
   run the install command in section 3 again after the build.
4. Point the existing entry to the current binary's absolute path and this
   checkout's absolute `--workspace` path, using section 3 as the reference.
   Remove an obsolete RTE MCP entry that still launches another binary.
5. Stop and restart RTE Studio, then restart the MCP client so it starts the
   rebuilt server. Run the section 2 tool-list check. With Studio connected,
   check `rte_device_status`, `rte_signal_info`, and
   `rte_device_commands` against the running firmware. If the old tool list
   persists, check the configured executable path and restart the client.

Updating the host MCP does not flash either MCU. Firmware-dependent features
need firmware images containing their matching contracts; see `AGENTS.md` for
the interfaces to check before a main or coprocessor reflash.

## 1. Prepare and build

RTE needs CMake 3.24+, a C++20 compiler, Ninja, and Qt 6 with Core, Gui,
Widgets, Network, OpenGL, and OpenGLWidgets. The QtNodes submodule must be
present. If it is missing, initialize submodules before configuring CMake.
On this Linux checkout, run:

```bash
cd /absolute/path/to/RTE
git submodule update --init --recursive
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure -R 'RTECLI_mcp_flash_integration|RteCli|RTEStudio_session_stale'
```

The full build compiles the current `rte` MCP/CLI server, `RTEStudio`, the
automation and protocol libraries, and the other host targets. The tests
exercise MCP discovery, telemetry, commands, main UART bridge flashing, and
coprocessor DFU flashing using simulated ports, plus Studio's stopped-signal
reporting through a local session; they do not touch hardware.
If the build or tests fail, fix that before configuring a client. If the
client was already configured, restart it after rebuilding so it launches
the new binary. Restart RTE Studio too so its Runtime console shows MCP
activity from the new server.

The server executable for this build is `build/bin/rte`. On a multi-config
generator, use the actual configuration-specific executable path (for
example `build/bin/Release/rte`). Resolve it to an absolute path before
placing it in client configuration.

## 2. Check the server without hardware

Use a short stdio MCP request to verify that the new executable starts and
advertises the expected tools:

```bash
python3 - <<'PY'
import json
import pathlib
import subprocess

root = pathlib.Path.cwd().resolve()
server = subprocess.Popen(
    [str(root / "build/bin/rte"), "mcp", "--workspace", str(root)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True,
)
try:
    for number, method, params in (
        (1, "initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                           "clientInfo": {"name": "rte-setup-check", "version": "1"}}),
        (2, "tools/list", {}),
    ):
        server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": number,
                                       "method": method, "params": params}) + "\n")
        server.stdin.flush()
        answer = json.loads(server.stdout.readline())
        assert "error" not in answer, answer
        if method == "tools/list":
            names = {tool["name"] for tool in answer["result"]["tools"]}
            assert {"rte_device_mode", "rte_device_telemetry", "rte_device_trends",
                    "rte_device_history_export",
                    "rte_device_snapshot", "rte_build_info", "rte_signal_info",
                    "rte_control_status", "rte_device_commands", "rte_spike_capture",
                    "rte_device_command_response", "rte_flash"} <= names, names
            print(f"RTE MCP ready: {len(names)} tools")
finally:
    server.stdin.close()
    server.wait(timeout=5)
    assert server.returncode == 0, server.stderr.read()
PY
```

MCP uses newline-delimited JSON-RPC on stdin/stdout. Keep stdout free for
protocol messages when running `rte mcp` directly.

## 3. Install and configure a client

Point the client at the **absolute path of the binary just built**, and pass
the **absolute repository path** with `--workspace`. Merge the entry into an
existing configuration; preserve other MCP servers. Replace the two example
paths below with the paths on the machine that is connected to the inverter.

### Codex CLI and ChatGPT desktop

Add this table to `~/.codex/config.toml` or a trusted project's
`.codex/config.toml`:

```toml
[mcp_servers.rte]
command = "/absolute/path/to/RTE/build/bin/rte"
args = ["mcp", "--workspace", "/absolute/path/to/RTE"]
tool_timeout_sec = 600
```

Restart the client. In Codex CLI, use `codex mcp list` or `/mcp` to check the
connection. ChatGPT desktop shares the Codex MCP configuration on the same
host. See the [official OpenAI MCP setup](https://learn.chatgpt.com/docs/extend/mcp).

### Kimi Code

Add an `rte` entry under `mcpServers` in `~/.kimi-code/mcp.json` or a trusted
project's `.kimi-code/mcp.json`:

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

Restart Kimi Code and use `/mcp` to check the connection. See the
[Kimi Code MCP setup](https://www.kimi.com/code/docs/en/kimi-code-cli/customization/mcp.html).

### Optional local installation

After the full build, `cmake --install build --prefix "$HOME/.local"` installs
the binaries and data. If using that copy, point the client at the absolute
path to `$HOME/.local/bin/rte`. Rebuild **and reinstall** after source changes;
otherwise the client will keep starting an older installed binary. Using
`build/bin/rte` during development avoids that extra step.

ChatGPT web cannot start this local stdio server. It needs a hosted MCP
connection or a supported tunnel; the configurations above are for local
Codex, ChatGPT desktop, and Kimi Code.

## 4. Use the tools

Gen7 current acquisition publishes the completed ADC burst before executing
the generated `adc_isr` graph, after base-image overcurrent protection. The
ADC domain uses the actual update-period time step; ADC and TIM1 graph dispatch restore
the interrupted domain's time step. This feedback-timing correction requires
a **main MCU rebuild and reflash**. Command and telemetry formats are unchanged;
no coprocessor reflash is needed.

- Use `rte_project_info`, `rte_graph_read`, and `rte_validate` to inspect a
  graph. `rte_graph_read` returns a compact node summary by default. Select
  `section` (`nodes`, `connections`, `bridges`, or `node_types`) and use
  `filter`, `offset`, and `limit` to inspect only the relevant entries. Full
  node-type source is omitted by default; request `include_code: true` with a
  narrow filter and limit when that implementation is specifically needed.
  Complete graph resources can exceed the MCP context budget and will be withheld with
  instructions to use the query tool. `rte_generate` and `rte_build` create
  firmware artifacts and return a bounded result instead of compiler progress.
- Use `rte_bridge_ports` to find the Gen7 `if00` UART bridge and `if02`
  coprocessor control port. Use `rte_device_mode` for a read only mode check.
  `app_responding` and `bootloader_responding` confirm replies; an
  `app_unresponsive_or_silent` result does **not** prove a hung MCU. The
  optional `probe_bootloader` argument makes a connect only programmer probe.
- For live data, run `RTEStudio` connected to the inverter first. Use
  `rte_device_status`, filtered `rte_signal_info`, `rte_device_signal`, or
  `rte_device_snapshot` first. `rte_device_telemetry` accepts exact `signals`
  or a substring `filter`, returns at most 50 values by default, and omits
  duplicate source-node groups unless `include_groups` is true. Follow
  `next_offset` only when another page is needed. After two seconds without a
  new report, a signal's current `value` becomes `null`; its `state` is
  `stopped_reporting` when the link is still active, or `link_silent` when
  telemetry frames have stopped. The old measurement remains in `last_value`
  or `last_known_values`, never as a fabricated zero. A stopped report does
  not by itself prove why the ISR stopped. With a matching Gen7 main image,
  TIM1 remains a permanent measurement and telemetry ISR after `control stop`
  and control faults. `control_state` describes the supervisor,
  `control_outputs_enabled` says whether graph PWM writes are permitted, and
  `tim_isr_running` independently describes the ISR. Manifest signals in the
  `tim_isr` domain remain `live` while fresh when control is `IDLE` or `FAULT`,
  and become `isr_stopped` only when firmware explicitly reports the ISR
  stopped. A two-second `stopped_reporting` fallback catches missing samples
  while the ISR claims to run. Legacy `foc stop` still publishes
  `foc_running=0`, marking native FOC controller outputs `foc_stopped`. These
  semantics require a **main MCU reflash**; older images retain the prior
  control-state fallback. Firmware build identity fields
  (`fw_manifest`, `fw_graph`, `fw_graph_hash`) are normally republished every
  ten seconds and use a 15-second signal timeout while the link remains live.
  `rte_device_histories` reads up to eight signals from one Studio store
  snapshot, with a total MCP budget of 400 raw samples; each sample retains
  its own timestamp, and its window continues advancing after reports stop.
  `rte_device_trends` analyzes the live numeric time
  series locally, not the graph JSON: 48-bin sparklines plus slope and fit,
  early/late
  mean and RMS change, standard deviation, steps, isolated spikes, approximate
  resolved oscillation frequency, pairwise correlation over shared time bins,
  sample rate, and sampling gaps. Its text
  names the observed pattern; structured metrics retain the evidence. Call
  it before and after a motor adjustment with the same signals and window
  to compare behavior. Correlation is a timing clue, not proof that one signal
  caused another. Use `rte_device_histories` for a small raw sample inspection,
  or `rte_device_history_export` to write up to eight longer histories to a
  local long-form CSV without feeding those samples to the model. A `limited`
  quality flag means the
  observed samples or window coverage do not support a strong conclusion;
  mean and RMS are sample based and do not interpolate gaps. Use
  `rte_device_console` for received lines.
- `rte_build_info` returns a compact running firmware identity and manifest
  counts by default. Request `detail: full` only when the full manifest is
  necessary and small enough for the response budget. `rte_signal_info` joins that
  catalog with observed signal freshness and units. `rte_control_status`
  reads state, ISR and generated-output enable state, latched fault names, PWM
  MOE, and gate status. These fields
  require a newly generated and flashed Gen7 main image; older images return
  unavailable metadata while ordinary telemetry still works.
- `rte_spike_capture` sends `spikes` to dump the firmware's existing frozen
  64-sample current/encoder capture with firmware-reported timing and re-arm it. It returns compact
  current/angle trend analysis by default. Set `include_samples: true` only
  when all 64 parsed samples are needed. It reports no capture if the
  current threshold has not fired. Set that threshold with the ordinary
  inverter command `spikes <amps>` if needed.
- Call `rte_device_commands` to discover the commands actually registered in
  the connected firmware. It sends `help`, waits for the closing reference
  line, and returns command names, usage, descriptions, argument ranges, and
  explicit motor control roles. Use `control start` to run the main motor
  control implemented by the loaded RTE graph. The separate
  `foc start <iq_a> [id_a]` path runs the base image's native FOC diagnostic
  for internal testing. Do not use `foc start` as the normal way to run the
  graph based motor controller. The two paths are mutually exclusive.
  Newly built Gen7 firmware also announces the registered command count so
  the tool can verify that no command entry was lost; older images report
  `count_verified: false`.
  Results default to 50 commands; use `filter`, `offset`, and `limit` if a
  future firmware exposes a larger command set. Discovery and command-count
  verification still cover the complete firmware `help` response before the
  returned page is selected.
  An incomplete response is reported as an error with the partial list. As
  with other agent-sent inverter commands, Studio must allow external command
  writes for this call.
- The same distinction appears for a person using Studio: enter `help` in the
  Runtime console to see `control` labeled as the main motor control and `foc`
  labeled as the internal test diagnostic. A main MCU image rebuilt from this
  source also prints that role when either path starts or reports status.
  Updating this wording requires a main MCU reflash; it does not require a
  coprocessor reflash.
- On current Gen7 main firmware, use `fault` as the canonical command for fault
  operations: `fault status`, `fault sources`, `fault clear [all|warning|high|critical|source]`,
  and `fault test <source>`. A bare `fault clear` resets all software fault
  latches, related gate/PWM and MAX22530 hardware latches, and the generated
  controller fault state. It refuses the gate reset while control or PWM is
  active. `fault reset`, `clear fault`, and legacy `clearfault` are aliases for
  the same clear routine. A persistent live condition is reported again after
  the clear.
- Use `rte_device_command` to send any inverter text command and get a console
  cursor, or `rte_device_command_response` to send and collect subsequent
  lines. The protocol does not associate each console line with a request;
  unrelated output can be included. RTE Studio must have **Preferences →
  Automation → Allow CLI and MCP clients to send commands to the device**
  enabled for command writes and Studio-coordinated flashing.
- Keep RTE Studio's Runtime console visible when operating hardware. It shows
  `[MCP]` tool and resource reads with their arguments and write completion status, followed by
  `[MCP] > ...` for the exact inverter command and `[MCP] sent` or a send
  failure. CLI commands use `[CLI]`. Repeated telemetry, status, history, and
  console reads are summarized at most once every five seconds per tool so
  the console remains usable. These lines are also kept in session exports.
- `rte_flash` targets the main MCU over the Gen7 bridge by default. Set
  `target: "coproc"` for USB DFU; the coprocessor must already be in DFU
  mode. `firmware` must name an existing `.elf`, `.hex`, or `.bin` image. The
  MCP result is deliberately compact: success returns one confirmation and
  failure returns one bounded error message. STM32CubeProgrammer byte dumps,
  percentages, and other progress lines are consumed by the wrapper and are
  never returned to the agent. Studio's local flash screen still shows its
  progress bar and log for a person operating the hardware.
- `rte_sim` remains exposed for simulator development, but simulation is
  **not correctly implemented** and is **not advised** for control tuning,
  firmware validation, or hardware decisions.

## Manual use in RTE Studio

Every MCP action has a manual route in Studio. The Runtime tab's live plots,
signal table, and console show signal trends, current values, and received
inverter replies. The **FOC** plot preset follows either `cg_` or native
`foc_` signals. **Inspect Firmware & Signals…** shows the manifest, fault
and controller status, and signal ages. Send inverter text through the
Runtime console; the exact command and response appear there.

For host actions, use **Build → Run RTE Command…**. Enter arguments after
`rte`; Studio runs its adjacent `rte` binary without a shell and shows output
and exit status. Examples:

```text
device mode
device build-info
device signals --filter foc
device control-status
device snapshot --bundle foc
device histories --signal cg_id_a --signal cg_iq_a --window-s 5
device ports
tool rte_device_trends --arguments '{"signals":["cg_id_a","cg_iq_a"],"window_s":5}'
tool rte_device_telemetry --arguments '{"filter":"current","limit":25}'
tool rte_device_history_export --arguments '{"signals":["cg_id_a","cg_iq_a"],"output":"captures/foc.csv","limit":12000}'
tool rte_device_commands
tool rte_spike_capture
flash --firmware /absolute/path/to/main.elf
trace record --interface can0 --output /tmp/capture.rte --seconds 10
```

The same command runner accepts `validate`, `generate`, `build`, `flash`,
`trace`, `sim`, and legacy `mcp2221` commands. Studio also has its normal
graph editor, Build menu, and Firmware Update screen for those common jobs.
`tool NAME --arguments JSON` invokes any MCP tool manually through the same
implementation and returns its text output; use `--format json` before `tool`
to see the full structured response. Manual inverter commands from `tool`
are labeled `[CLI]` in the Runtime console.
`rte_device_command_response` corresponds to sending a command in the
Runtime console and watching its subsequent lines; neither route can prove
that a particular line belongs to that command.
For the high-rate spike capture, send `spikes` in the Runtime console; use
`spikes <amps>` to change its trigger threshold.
Send `help` in that console to see the same live command reference returned by
`rte_device_commands`.

## 5. Keep model context bounded

Use a narrow-to-detailed sequence: device and control status, filtered signal
discovery, an exact snapshot, then `rte_device_trends`. Request bounded raw
history only to inspect individual timestamps, and export larger datasets to
CSV for external scripts or plotting. Telemetry, signal catalogs, graph
sections, console lines, and histories have conservative defaults and hard
limits. Responses at or above 16 KiB carry a warning; responses above 32 KiB
are withheld with a refinement message. A completed command or flash is marked
completed even if oversized details are withheld, so do not repeat a hardware
action just to retrieve its discarded log.

The CSV export path must end in `.csv` and remain inside the configured
workspace. The MCP response contains file location, sample counts, and summary
statistics; the raw rows stay in the file. Agents may use local Python or a
numeric library on that file when custom analysis is more appropriate than the
built-in trend metrics.

The full CLI and hardware details are in
[docs/automation-backend.md](docs/automation-backend.md).

## Gen7 current-control capture and voltage units (2026-09-22)

The current-loop repair requires a main MCU rebuild/reflash and a rebuilt `rte`
for spike timing support. It does not change the binary telemetry protocol or
require a coprocessor reflash. `spikes` format 2 reports measured sample rate and
raw cycle timestamps; the MCP parser supports both this and the older 5 kHz
format. The older header does not prove the actual ADC rate on legacy firmware.

`cg_vd_v` / `cg_vq_v` now report the vector-limited physical-voltage commands;
`cg_vd_req_v` / `cg_vq_req_v` report the requests before the shared vector limit.
`cg_vlimit_scale` is 1 when not clipped. `cg_sample_age_us`, `cg_sample_valid` and
`cg_sample_seq` describe the single current frame latched for each TIM step.
The existing Kp/Ki/feedforward settings retain their effective actuation through
an explicit 0.5 legacy-to-volts adapter. `Ctrl.VoltLimitPu` defaults to 1/3 for
the first comparison, capped by the linear modulation and ADC-window margins.

Use `ctrlcap arm [decimation]` to collect a bounded 512-frame generated-control
capture; `ctrlcap status` reports progress. Once frozen, `ctrlcap dump [offset]
[count]` reads at most 16 frames per page without rearming. `ccA` and `ccB` CSV
lines share an index; headers name every column. Cycles are raw DWT counts:
subtract as unsigned 32-bit values before dividing by `clock_hz`. The recorder
runs from TIM control; ADC `seq` identifies repeated feedback samples. Age is
measured from ADC callback entry, not a claimed analog acquisition timestamp.
Tracked active duties are software estimates of timer-latched commands, not
measured gate voltages. Frame storage is approximately 60 KiB in AXI SRAM.

The same commands work in the Studio Runtime console and through
`rte_device_command_response`. Export pages to a local file for analysis. Keep
MCP's response guard and bounded pagination; do not dump all 512 frames in one
response. `control start` runs graph control; native `foc start` remains a
separate internal diagnostic.

### Final 75 V acquisition update (2026-09-23)

The injected ADC now uses TIM1 TRGO2 update events at both PWM extrema: 5 kHz
during graph control with 2.5 kHz switching. Raw overcurrent checks remain in
the ADC path. Each raw sample is Park-transformed at its own angle, then the
controller uses a coherent 1/4, 1/2, 1/4 full-cycle d/q estimate (200 us delay).
`cg_id_a` / `cg_iq_a` report this actual controller feedback; `cg_id_raw_a` /
`cg_iq_raw_a` expose the newest unaveraged d/q sample. `ctrlcap` ccA headers now
include appended `raw_id,raw_iq` columns; phase currents and theta refer to that
newest raw frame. Parse columns by the emitted headers. Sample age describes
the newest ADC callback, not the older estimator center. The recorder rate
comes from its timestamps. Legacy OC4 scheduling no longer selects the injected
trigger; both zero-vector windows must satisfy the guard.

The tested FRAM voltage fraction is 0.45; graph fallback remains 1/3. See
`docs/gen7-current-loop-results.md` for measurements, the flashed image hashes,
remaining raw ripple and serial/host-reload limitations. No permanent RPM cap
was introduced. Full voltage utilization and full-speed operation are unvalidated.

### Gen7 control speed (2026-09-24)

Newly generated `foc_demo` images expose `cg_rpm_control`, the DMA-derived
mechanical speed used by voltage feedforward and angle prediction. Existing
`Mech_RPM` / `Elec_RPM` remain dashboard estimates and can lag acceleration by
approximately one second. Request `cg_rpm_control` explicitly in telemetry,
trends, or Studio's signal plot when examining control timing. This addition
is discovered through the ordinary runtime signal manifest; no new MCP tool
or coprocessor flash is needed. A main MCU rebuild/reflash is required.
Switching remains 2.5 kHz. See `docs/gen7-control-speed-results.md` for bench
results, residual ripple, rejected gain trials and the latest configuration.
