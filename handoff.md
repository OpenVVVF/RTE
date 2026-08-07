# Agent handoff — HostSim live telemetry & OpenVVVF PR

**Date:** 2026-07-30 (local)  
**Workspace:** `C:\Users\bc200\.cursor\STMSTUFF`  
**Active branch:** `feat/hostsim-live-telemetry` (tracks `fork/feat/hostsim-live-telemetry`)

---

## Primary deliverable

**Open PR:** [OpenVVVF/RTE#3 — feat: HostSim live telemetry, PWM scope, and SPWM demo for NodeGUI](https://github.com/OpenVVVF/RTE/pull/3)

- **Head:** `hibyemy:feat/hostsim-live-telemetry`  
- **Base:** `OpenVVVF/RTE` `main`  
- **State:** OPEN — conflicts with `main` were resolved locally and pushed (`654bfb0`, `4f77c82`). Re-check GitHub mergeability before merge (was **clean** after last push).

**Other open PR (same fork, unrelated stack):** [PR #2 — Nucleo-L476RG base image](https://github.com/OpenVVVF/RTE/pull/2). PR #3 branch history includes HostSim + nucleo-era commits; reviewers may want ordering clarified.

---

## What this branch adds (summary for reviewers)

### HostSim (`Images/HostSim/`)

- Live **InverterProtocol (IVP) TCP** telemetry (`telemetry_publisher`, port 14608, `--live`)
- **PWM scope** optional simulator (`pwm_scope.cpp`) — triangle carrier, `pwm_gate_*`, `pwm_v_*`
- **`pwm_scope_enabled` defaults to `false`** unless scenario JSON has `"pwm_scope"` (VVVF-friendly `default_motor.json` has no block)
- **SPWM demo:** `graphs/spwm_demo_graph.json`, `scenarios/spwm_demo.json`, `scripts/run_spwm_live.ps1`
- **`platform_spwm_step()`** and related API in `platform_api.h/cpp`
- ODE **PMSM plant** in `motor_model.cpp` — **not ngspice** (averaged duty → dq Euler step)

### InverterProtocol (`Lib/InverterProtocol/`)

- **TCP transport** for HostSim ↔ NodeGUI
- **Multi-frame COBS fix** in `host_client.cpp` (connect/disconnect loop)

### NodeGUI (`Source/NodeGUI/`)

- **`--tcp host:port --protocol ivp`** for HostSim live; **legacy UART remains default**
- HostSim enhancements kept: edge-preserving `pwm_gate_*` ingress, pause/freeze plots, SPWM preset, `SimSpeedControl`, Win32 `FirmwareUpdater_win` / `HttpApiServer_win` stubs
- **Merged upstream `main`:** top-level **QTabBar + QStackedWidget** (Node Editor / Runtime / Firmware Update), **Build → Generate/Flash** (`code gen` commit), console **Logs** tab — see conflict resolution below

---

## Merge conflict resolution (already done)

1. **First merge (`654bfb0`):** Runtime `src/runtime/*` add/add → kept **ours** (HostSim live path). **MainWindow** → upstream 3-screen shell + our TCP/SPWM (`SetupRuntime` tcp args, `FindSpwmDemoGraph`, File → Open SPWM Demo). **RuntimeTab** → upstream ctor (no embedded Flash); Firmware Update is top-level screen.

2. **Second merge (`4f77c82`):** Upstream moved again (`b194f82 code gen`). Merged **both** `FindSpwmDemoGraph` and main’s `ProjectRoot` / `BuildKeyForGraph`; kept build-log console dock comment from main.

**Build verified after merge:** `cmake --build build --target NodeGUI` succeeded on Windows.

---

## Local git state (important)

```text
## feat/hostsim-live-telemetry...fork/feat/hostsim-live-telemetry
 M Images/HostSim/README.md
 M README.md
?? Images/HostSim/docs/          # Implementation_Plan.md (ngspice roadmap)
```

**Not committed yet:** ngspice **implementation plan** doc + README links. User asked to plan only — **commit/push only if user asks**.

**Stash:** `stash@{0}: wip NodeAPI and local dumps` — contains unstaged `Lib/NodeAPI/*` and local debug files. Do not pop blindly; may conflict.

**Excluded from PR intentionally:** `AUDIT_Claim_vs_Implementation.md`, `live_out.txt`, `telem_dump.txt`, `trace_spwm.csv`, `Lib/NodeAPI/fw_test.bin`, etc.

**Local `main`:** may show `behind origin/main`; work happens on **`feat/hostsim-live-telemetry`**, which already merged upstream `main`.

---

## How to run (smoke test)

**SPWM live (recommended demo):**

```powershell
powershell -File Images\HostSim\scripts\run_spwm_live.ps1
```

Runtime screen → **SPWM** preset; Graph 1 ~40 ms for gates; **Resume** if paused.

**VVVF-style plant only (no PWM scope flood):**

```powershell
Images\HostSim\build\Debug\host_sim.exe --scenario Images\HostSim\scenarios\default_motor.json --live --tcp 127.0.0.1:14608
build\Source\NodeGUI\NodeGUI.exe --tcp 127.0.0.1:14608 --protocol ivp
```

**Build (from repo root):**

```powershell
cmake --build build --config Debug --target NodeGUI
cmake --build Images/HostSim/build --config Debug
```

---

## VVVF / compatibility (for PR narrative)

| Topic | Status |
|-------|--------|
| `platform_pwm_set` / `platform_pwm_set_voltage_vector` | Unchanged |
| Real hardware path | Legacy UART default in NodeGUI |
| PWM scope | Scenario opt-in only |
| FOC example graphs (`Assets/Examples/foc_demo.json`) | Compatible |
| IVP/TCP | HostSim live only |

---

## Planned work (not implemented)

**ngspice plant backend** — documented only:

- [Images/HostSim/docs/Implementation_Plan.md](Images/HostSim/docs/Implementation_Plan.md)  
- Phases: `IPlant` extract → sharedspice → scenario `backend: ngspice`  
- Open decisions in §7 of that doc (RL vs hybrid, offline vs live, Windows vs Linux first)

Root `README.md` roadmap links to the same plan.

---

## Key paths

| Area | Path |
|------|------|
| HostSim runtime | `Images/HostSim/src/sim_runtime.cpp`, `main.cpp` |
| Telemetry | `Images/HostSim/src/telemetry_publisher.cpp` |
| PWM scope | `Images/HostSim/src/pwm_scope.cpp` |
| ODE plant | `Images/HostSim/src/motor_model.cpp` |
| NodeGUI bridge | `Source/NodeGUI/src/runtime/RuntimeController.cpp` |
| TCP IVP | `Lib/InverterProtocol/src/host/tcp_transport.cpp`, `host_client.cpp` |
| MainWindow shell | `Source/NodeGUI/src/MainWindow.cpp` |
| SPWM scenario | `Images/HostSim/scenarios/spwm_demo.json` |

---

## Suggested next tasks (pick based on user ask)

1. **Commit & push** `Images/HostSim/docs/Implementation_Plan.md` + README links (if user wants docs on PR).
2. **PR hygiene:** Confirm #3 mergeable on GitHub; update PR body if needed; respond to review.
3. **ngspice Phase 0–1:** `IPlant` interface + refactor ODE behind it (see Implementation_Plan.md).
4. **Do not** merge NodeAPI stash into telemetry PR without explicit user request.
5. **Optional:** `run_foc_live.ps1` for FOC + HostSim TCP (never built in this thread).

---

## User / process rules (short)

- **Do not git commit** unless the user explicitly asks.
- **Do not force-push** `main`; fork remote is `fork` → `https://github.com/hibyemy/RTE.git`.
- **Upstream:** `origin` → `OpenVVVF/RTE`.
- Prefer **minimal diffs**; match existing code style.

---

## Conversation context

Full prior agent transcript (PWM fixes, lag, connect loop, PR creation, conflict merge):

`C:\Users\bc200\.cursor\projects\c-Users-bc200-cursor-STMSTUFF/agent-transcripts/8739eaaa-8e22-4bf3-b4cc-fc5d61a0004c/8739eaaa-8e22-4bf3-b4cc-fc5d61a0004c.jsonl`

Search keywords: `hostsim-live-telemetry`, `pwm_gate`, `merge origin/main`, `PR 3`.
