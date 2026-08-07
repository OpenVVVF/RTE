# 2026-08-06 — Kimi Code: audit fixes, firmware-target selection, FPGA fundamentals doc

## Goal

User: fix all audit issues, make the firmware target selectable (Gen6FW /
NucleoL476FW / HostSim) instead of hardcoded, verify "Build & Reflect in
Simulator" with a real run, audit the 4 remaining modules, then explain the
FPGA work educationally (fundamentals, workflow split, per-module
implementation, duty split).

## What was done

### Fixes (build green, 91/91 tests)

- **GUI firmware target** (`PreferencesDialog.{h,cpp}`, `MainWindow.cpp`):
  Edit → Preferences → Build tab → "Firmware target" combo (Gen6FW /
  NucleoL476FW / HostSim), persisted in QSettings, threaded into the `rte` CLI
  `--base-source` via new `FirmwareBaseDir(target)` helper (Gen6FW default).
  Also fixed 3 BuildSimulation wiring defects: `setWorkingDirectory` after
  `start()`, `cliStage_` unset in one-shot path, stale `cliOutputBuffer_`.
- **InverterProtocol**: all stats writes under `stats_mtx_`; UART read failures
  now distinguishable (`readFailed()`) so the dead reconnect loop works;
  `writeRaw` 2 s WouldBlock deadline; `getaddrinfo` (localhost works); baud
  plumbed through on Windows+POSIX; named buffer constants; `reject_decode`
  wired; dead idle-check block removed.
- **NodeAPI/RTELogger**: `Connect` rejects double-connected inputs;
  `RemoveNodeType` refuses with live instances; `LoadIntoGraph` propagates
  failures; NodeTemplates loads top-level *.json, accurate counters, zero-types
  = failure, hardened fs/JSON errors; Logger thread-safe (mutex +
  localtime_r/s) + ParseLevel warns on garbage; deleted `fw_test.bin`.
- **Codegen/Emitter/Builder**: error_code filesystem handling with clean
  errors (incl. base-tree copy loop + iterator); Boolean params reject invalid
  spellings; unknown-typed param → error not silent `f` suffix; chmod 0644;
  out-of-range marker warns; MarkerParser accepts tabs; env override
  (`RTE_INVERTERCODEGEN_INCLUDE_DIR` / `_THIRD_PARTY_DIR`) for baked paths;
  dead variable in Toolchain.cpp.
- **Tools/launchers**: `migrate_graph_ids.py` dedup + template path restored
  (repo-root-relative); `can_session_client.py` length guards; `fram_keys.py`
  float round-trip (`.9g`), pyserial message, named constants; `sim_device.py`
  POSIX guard + rate validation; test sys.path file-relative; launchers use
  `%~dp0` + `%PORT%`, ping-sleep → timeout.
- **HostSim scripts**: emitter flag corrected `--templatesDir` → `--templates`
  (this was breaking Build & Reflect!); multi-config exe lookup
  (`Debug/host_sim.exe` etc.) in `run_spwm_live.sh`.
- **Emitter test fixtures**: declare `parameterTypes` for `constant.value` /
  `control.gain` (strict param check now rejects undeclared params).

### Build & Reflect verification (headless)

`run_spwm_live.sh --force-emit --no-gui`: emit → CMake/MSVC build → sim start
→ TCP 127.0.0.1:14608 serving IVP — **verified end-to-end** (connected with a
socket, then killed host_sim). Note: `pgrep` is missing on this Git Bash
install (cosmetic); the ps1 variant's WSL `/opt/rtehost` emitter path and its
`Stop-SimApps` killing processes named `NodeGUI` are pre-existing caveats
(our GUI binary is now `RTEStudio.exe`, so the kill no longer matches).

### Audit of remaining modules (NodeGUI runtime+core, Images, Assets)

- NodeGUI: Qt parent-child pattern throughout, no hardcoded paths/IPs — clean.
- Assets: all templates well-formed, no missing required keys — clean.
- Images: vendor HAL/CMSIS noise aside, one real finding — **`FpgaSpiDriver.cpp`
  is a stub**: returns simulated register values, `FpgaSpi_Init` reports
  `is_initialized=true` with no hardware. Intentional scaffold, but must not
  ship in a real flash; Phase 2 replaces it with real HAL SPI2 transfers.
- HostSim: port/host are named constants with CLI overrides — clean.

### Docs

- `docs/FPGA_FOC_FUNDAMENTALS.md` — educational doc: why hybrid, FOC on one
  page, Q15 fixed-point, MCU/FPGA duty split (incl. SPI bandwidth budget and
  CDC), per-module implementation guide (what exists/how it works/what's
  missing), verification workflow, suggested learning order.

## State at handoff

- Branch `feat/hostsim-live-telemetry`, build green, 91/91 tests, tree clean
  after the fix commits; pushed to `fork`.
- PR #3 (OpenVVVF/RTE) head updated; was MERGEABLE/CLEAN before these commits.

## Gotchas for the next agent

- The `rte` CLI flag is `--templates` (not `--templatesDir`); `templatesDir` is
  only a C++ member name. The HostSim ps1 still uses WSL for the emitter.
- GUI "Firmware target" = Gen6FW default; HostSim target makes sense mainly
  with Build & Reflect, less with plain Generate/Flash.
- `FpgaSpiDriver` stub: `FpgaSpi_ReadRegister`/`WriteRegister` return fake
  data — gate any real-motor use on replacing this.
- Subagent quota died mid-swarm twice this session; their tree edits were
  complete anyway, but always `git diff`-review leftovers after a failed swarm.
