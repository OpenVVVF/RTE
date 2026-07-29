# HostSim — upstream-compatible host simulator base image

Host-side base image for `RTECodeEmitter`. Generated domain code calls
`platform_api.h` only; motor plant, sensor injection, and scheduling live in
this base image.

## Domains

| Domain | Default rate | Host location |
|--------|--------------|---------------|
| `tim_isr` | 10 kHz | `SimRuntime::StepOnce()` |
| `adc_isr` | 10 kHz | `SimRuntime::StepOnce()` |
| `app_loop` | 1 kHz | `SimRuntime::StepOnce()` |

Rates are configurable in the scenario JSON (`scenarios/default_motor.json`).

## RTE_EMIT markers

| File | Markers |
|------|---------|
| `include/AppState.h` | `app_loop`, `tim_isr`, `adc_isr` **state** |
| `src/sim_runtime.cpp` | all three **init** and **step** |

## Quick start (base image only)

```powershell
cd Images\HostSim
cmake -S . -B build
cmake --build build
.\build\host_sim.exe scenarios\default_motor.json
```

Writes `trace.csv` with columns:
`time_us, throttle_a, throttle_b, duty_u, duty_v, duty_w, i_a, i_b, i_c, theta_e, omega_e`.

## Emit-and-run (graph + codegen)

From repo root (WSL for `RTECodeEmitter`):

```powershell
powershell -File Images\HostSim\scripts\emit_and_run.ps1
```

Or on Linux/macOS:

```bash
./Images/HostSim/scripts/emit_and_run.sh
```

This copies `Images/HostSim` to `build/hostsim_emitted`, runs `RTECodeEmitter` with
`baseline_graph.json`, and builds the emitted tree with CMake.

## Scenario file

Motor parameters are **not** hardcoded to a specific machine. Edit
`scenarios/default_motor.json` (or pass another file as argv[1]):

- `motor.*` — PMSM Rs, Ld/Lq, flux, pole pairs, inertia, Vdc
- `throttle_a` / `throttle_b` — `constant`, `ramp`, or `step` profiles
- `simulation.duration_s`, `trace_csv`, domain rates

A comment field documents where to paste calibrated values (e.g. 75-5 bench motor).

## Visualization

**Offline:** `python scripts/plot_sim.py trace.csv`

**Live (NodeGUI):** merged upstream runtime tab supports `LegacyTelemetryClient`
and `Protocol::Inverter` (`ivp::InverterClient`). HostSim currently logs
`platform_telemetry_log_f32` to stderr and CSV traces. To connect NodeGUI live,
add an InverterProtocol serial/TCP publisher in `platform_telemetry_log_f32` using
the same keys as graph telemetry nodes — no change to generated code required.

## Upstream compatibility

- Same `platform_api.h` surface as Gen6FW / NucleoL476FW (subset + stubs).
- Same three timing domains as Gen6 baseline graphs.
- No changes to `NodeAPI`, `InverterCodegen`, or `RTECodeEmitter` core.
- Intended for contribution back to `OpenVVVF/RTE` as `Images/HostSim/`.

## Dependencies

- CMake 3.24+, C++20 host compiler
- Python 3 + matplotlib (optional, for `plot_sim.py`)
- WSL/Linux build of `RTECodeEmitter` for emit-and-run scripts
