# RTE MCP: setup instructions for an AI agent

Use this file when asked to set up the RTE MCP server for Codex, ChatGPT
desktop, or Kimi Code. Work from this repository's root. **Build the current
source before configuring a client or starting the server.** Do not use an
older `rte` binary found on `PATH`.

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
ctest --test-dir build --output-on-failure -R 'RTECLI_mcp_flash_integration|RteCli'
```

The full build compiles the current `rte` MCP/CLI server, `RTEStudio`, the
automation and protocol libraries, and the other host targets. The tests
exercise MCP discovery, telemetry, commands, main UART bridge flashing, and
coprocessor DFU flashing using simulated ports; they do not touch hardware.
If the build or tests fail, fix that before configuring a client. If the
client was already configured, restart it after rebuilding so it launches
the new binary.

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
            assert {"rte_device_mode", "rte_device_telemetry",
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

- Use `rte_project_info`, `rte_graph_read`, and `rte_validate` to inspect a
  graph. `rte_generate` and `rte_build` create firmware artifacts.
- Use `rte_bridge_ports` to find the Gen7 `if00` UART bridge and `if02`
  coprocessor control port. Use `rte_device_mode` for a read only mode check.
  `app_responding` and `bootloader_responding` confirm replies; an
  `app_unresponsive_or_silent` result does **not** prove a hung MCU. The
  optional `probe_bootloader` argument makes a connect only programmer probe.
- For live data, run `RTEStudio` connected to the inverter first. Then use
  `rte_device_status`, `rte_device_telemetry` for all latest numeric and
  string values, `rte_device_signal` for one value, and the two history
  tools for samples over time. Use `rte_device_console` for received lines.
- Use `rte_device_command` to send any inverter text command and get a console
  cursor, or `rte_device_command_response` to send and collect subsequent
  lines. The protocol does not associate each console line with a request;
  unrelated output can be included. RTE Studio must have **Preferences →
  Automation → Allow CLI and MCP clients to send commands to the device**
  enabled for command writes and Studio-coordinated flashing.
- `rte_flash` targets the main MCU over the Gen7 bridge by default. Set
  `target: "coproc"` for USB DFU; the coprocessor must already be in DFU
  mode. `firmware` must name an existing `.elf`, `.hex`, or `.bin` image.
- `rte_sim` remains exposed for simulator development, but simulation is
  **not correctly implemented** and is **not advised** for control tuning,
  firmware validation, or hardware decisions.

The full CLI and hardware details are in
[docs/automation-backend.md](docs/automation-backend.md). This guide describes
local MCP setup; it does not require changing either firmware image.
