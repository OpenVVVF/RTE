# HostSim — upstream-compatible host simulator base image

Host-side base image for `RTECodeEmitter`. Generated domain code calls
`platform_api.h` only; motor plant, sensor injection, and scheduling live in
this base image.

## Architecture

HostSim is a **software-in-the-loop (SIL)** simulator, not an interpreter:

```text
Node graph JSON
      │
      ▼
RTECodeEmitter ──► generates C++ for tim_isr / adc_isr / app_loop
      │
      ▼
cmake --build ──► host_sim executable
      │
      ▼
compiled control loop ──► platform_api.h ──► plant backend
      │                                          │
      │          ┌───────────────────────────────┘
      │          ▼
      │    OdePlant (default) ── discrete PMSM ODE
      │    NgspicePlant (experimental) ── libngspice circuit sim
      │
      ▼
telemetry → NodeGUI / CSV
```

The control code inside `host_sim` is the **same compiled firmware** that runs
on the STM32 images. The only difference is the base image underneath it:
HostSim provides simulator implementations of `platform_pwm_set()`,
`platform_get_phase_currents()`, etc., instead of STM32 HAL drivers.

The default plant is a fast discrete PMSM ODE (`src/motor_model.cpp`). An
optional **ngspice** backend exists (`src/plant/ngspice_plant.cpp`) but is
experimental and not yet the default.

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
- `simulation.demo_fallback` — opt-in legacy open-loop SPWM synthesized by the
  scheduler when no graph node drives `platform_pwm_set` (default **false**;
  when off and duties stay at 0 despite non-zero throttle, HostSim logs a
  warning once). Only `default_motor.json` enables it, for plant bring-up
  without a graph.
- `simulation.config_file` — backing file for the `platform_config_*` key/value
  store (`key=value` lines, preloaded at startup, flushed on every set);
  absent = in-memory only.
- `adc.*` — phase-current ADC model error terms; absent = ideal behaviour.
  `resolution_bits` (16), `vref_v` (3.3), `ref_v` (1.65), `divider` (2/3),
  `sensitivity_v_per_a` (1.042e-3), `gain_error` (1.0), `offset_u_a`,
  `offset_v_a`, `noise_std_a` — defaults in parentheses come from the Gen6
  signal-chain constants in `include/RteParams.h`.
- `environment.*` — `motor_temp_c` (25), `inverter_temp_c` (25) surfaced by the
  platform temperature APIs.
- `faults.*` — simple triggers surfaced via `platform_has_critical_fault()`:
  `overcurrent_a` (trip when any |i_phase| exceeds), `undervoltage_v` (trip
  when the DC link drops below), `vdc_glitch_time_s` + `vdc_glitch_v` (timed
  DC-link drop applied to both control code and plant).
- `can.*` — `loopback` (default true: frames sent via `platform_can_send` are
  readable via `platform_can_rx`, latest-frame store keyed by (bus, id)) and
  `frames`: scheduled injected traffic, e.g.
  `{"bus": 1, "id": 291, "period_s": 0.01, "start_s": 0.1, "data": "DEADBEEF"}`
  (numeric id accepts decimal or `0x` hex; `data` is a hex string, ≤ 8 bytes;
  `period_s` > 0 repeats, otherwise single shot at `start_s`).

A comment field documents where to paste calibrated values (e.g. 75-5 bench motor).

## Platform coupling

`platform_phase_voltage_u/v/w()` read the terminal voltages the plant actually
applied on its last step (recorded by the ODE motor model from its clamped
duty×Vdc drive), not the requested duties — so telemetry reflects a Vdc glitch
or a live duty override automatically. ADC injected channel reads are
conversion-latched per `adc_isr` tick; CAN `rx`/`send` follow the Gen6
latest-frame semantics; `platform_critical_enter/exit` are a real recursive
mutex.

## SPWM demo (NodeGUI + HostSim live)

Open-loop **sinusoidal PWM** graph for the host simulator. Throttle A sets modulation
index (0..1), throttle B maps to electrical frequency (1..20 Hz via the graph).

```powershell
powershell -File Images\HostSim\scripts\run_spwm_live.ps1
```

This emits `graphs/spwm_demo_graph.json`, builds `build/hostsim_spwm_emitted`, starts
HostSim live, and opens NodeGUI with the graph loaded.

**Suggested Runtime plots (check G1/G2/G3):**
- `duty_u`, `duty_v`, `duty_w` — slow SPWM duty commands (%)
- `pwm_gate_u`, `pwm_gate_v`, `pwm_gate_w` — switched gate outputs (0/1, scope-style)
- `pwm_v_u`, `pwm_v_v`, `pwm_v_uv` — phase / line-line voltages (V, vs DC-)
- `i_a`, `i_b`, `i_c` — simulated motor currents
- `spwm_angle_deg`, `encoder_angle_deg` — field angle vs rotor angle
- `mod_index`, `elec_freq_hz` — live control inputs

PWM scope uses a triangle carrier (default 800 Hz in `spwm_demo.json`). Telemetry
stays at `telem_hz` (500 Hz default); raise it in the scenario if you need
finer PWM resolution. NodeGUI decimates bursts and refreshes plots at ~30 Hz.

Edit graph parameters in NodeGUI (`FreqMin`/`FreqMax`, `TimDt`) then re-run the script
to regenerate firmware.

```powershell
.\build\Debug\host_sim.exe scenarios\default_motor.json
python scripts\plot_sim.py trace.csv
```

**Live dashboard (Path A — NodeGUI Runtime tab):**

```powershell
# Terminal 1 — long-running HostSim with InverterProtocol over TCP
.\build\Debug\host_sim.exe scenarios\default_motor.json --live --realtime 1.0

# Terminal 2 — NodeGUI Runtime tab connected to HostSim
.\build\Source\NodeGUI\NodeGUI.exe --tcp 127.0.0.1:14608 --protocol ivp
```

In the NodeGUI **Runtime** console, adjust live:

```text
throttle a 0.5
throttle b 0.0
clear
quit
```

## Windows live-mode tuning

HostSim live mode runs a 10 kHz simulation loop. On Windows, smooth pacing needs
help from the OS scheduler:

**Built-in (automatic in `--live` mode):**
- 1 ms multimedia timer resolution (`timeBeginPeriod`)
- Elevated thread priority + MMCSS `Pro Audio` class
- Wall-clock pacing every 1 ms sim time (not every 100 µs step)
- Hybrid sleep + short spin-wait for sub-millisecond accuracy

**Manual OS tweaks (recommended on laptops):**
1. **Power plan** — set Windows to *High performance* or plug in AC power.
2. **Close heavy apps** — browsers/GPU tools competing for the same cores.
3. **Exclude from Game Bar capture** if recording causes stutter.
4. **Start HostSim before NodeGUI** so the sim claims a performance core first.

**If plots still stutter:**
- Use `--realtime 0` on HostSim to run as fast as possible (no wall-clock pacing).
- Lower plot window (e.g. 5 s instead of 10 s) in the Signals panel.
- Set `HOSTSIM_TELEM_STDERR=1` only when debugging — stderr logging is off by default.

**Waveform sampling (live plots):**
- Default live telemetry is **500 Hz** (was 100 Hz) so duty/current waveforms have enough points per cycle.
- Rule of thumb: `telem_hz` ≥ 10× your highest electrical frequency (e.g. 20 Hz → use ≥200 Hz).
- Override with `--telem-hz 1000` or `"telem_hz": 1000` in the scenario JSON.
- Use **Pause Sim** in the Runtime console to freeze the plant while inspecting a trace.
- **Slow motion:** set sim speed to `0.25x` / `0.5x` in the Runtime console, or `speed 0.25` on the HostSim shell. `1x` = realtime, `turbo` = as fast as possible.

Built-in streamed keys: `throttle_a`, `throttle_b`, `duty_u/v/w`, `i_a/b/c`,
`theta_e`, `omega_e`, `vdc_v`, plus any `platform_telemetry_log_f32` keys from
the emitted graph.

`platform_telemetry_log_f32` still prints to stderr and also registers into the
live IVP publisher when `--live` is active.


## Upstream compatibility

- Same `platform_api.h` surface as Gen6FW / NucleoL476FW (subset + stubs).
- Same three timing domains as Gen6 baseline graphs.
- No changes to `NodeAPI`, `InverterCodegen`, or `RTECodeEmitter` core.
- Intended for contribution back to `OpenVVVF/RTE` as `Images/HostSim/`.

## Roadmap

- **Today:** ODE PMSM plant is the default and only fully-supported backend.
- **Experimental:** ngspice plant backend (`src/plant/ngspice_plant.cpp`) is
  present but not complete. Select it with `"plant": { "backend": "ngspice",
  "netlist": "plants/your.cir" }` in the scenario JSON. If `libngspice.so` is
  missing or loading fails, HostSim falls back to `OdePlant`.
- See [docs/Implementation_Plan.md](docs/Implementation_Plan.md) for the full
  ngspice integration plan.

## Dependencies

- CMake 3.24+, C++20 host compiler
- Python 3 + matplotlib (optional, for `plot_sim.py`)
- WSL/Linux build of `RTECodeEmitter` for emit-and-run scripts
