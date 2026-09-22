# Agent guidance for RTE

This repository includes an MCP server in `Source/RTECLI/src/Main.cpp` (`rte mcp`),
backed by RTE Studio's live session in `Source/NodeGUI/src/runtime/LocalSessionServer.cpp`.
Read `MCP_AGENT_SETUP.md` before configuring or using it. Build the current `rte`
and `RTEStudio` binaries before testing changes; an old installed binary can hide
source changes.

## Keep firmware and MCP compatible

When changing inverter or coprocessor firmware, check its host-facing contract
before calling the change done. The generic `rte_device_command` and
`rte_device_command_response` tools send arbitrary command text. The
`rte_device_commands` catalog is discovered from the running main firmware's
`help` output, so new registered commands need no new MCP tool **if the help
format stays compatible**. Numeric and string telemetry are discovered from
received frames; generated Gen7 firmware also announces a graph and signal
manifest. New signal names can be requested without adding a host tool.

The following behavior is coupled to firmware and needs a coordinated host
change and test when its contract changes:

- Command names, argument semantics, and `help` line format: command registry
  and `CommandManager::printHelp()` in the main image; MCP command discovery
  and its parser in `Source/RTECLI/src/Main.cpp`.
- Telemetry frame IDs, encoding, framing, or UART mode: `Lib/InverterProtocol`,
  the main image, the coprocessor bridge, and the Studio receive path.
- Build manifest format and control/fault signal names: generated build info,
  main firmware publishers, Studio's catalog/status endpoint, and MCP clients.
- FOC signal names: Studio's FOC plot and snapshot preset, plus the MCP trend
  tool's default FOC selection. Explicit signal lists continue to use runtime
  names.
- `spikes` command or capture text: the main recorder and the MCP
  `rte_spike_capture` parser, including its assumed sample count and rate.
- Bridge control commands, USB port identity, bootloader entry, or flash
  settings: coprocessor firmware and `Source/RTEAutomation` flashing/mode code.

For those changes, update the matching host code, `MCP_AGENT_SETUP.md` and
`docs/automation-backend.md` as needed, and the relevant integration tests.
Build the host tools and run
`ctest --test-dir build --output-on-failure -R 'RTECLI_mcp_flash_integration|RteCli'`.
Check `rte_device_commands`, `rte_signal_info`, `rte_build_info`, and the
affected specialized tool against a connected device when hardware is
available. The simulated-port tests do not prove compatibility with a newly
flashed image. Document which main or coprocessor image must be reflashed.

The MCP server and Studio CLI expose the same host actions; keep manual
routes in Studio available when adding an MCP action.
