# Simulation

RTE ships two host simulators, both living under `Images/`:

- **HostSim** (`Images/HostSim/`) — a *base image* for the code emitter,
  targeting the host instead of the STM32. `RTECodeEmitter` copies it,
  generates domain code from your graph, and builds a `host_sim` executable
  that runs the graph's `tim_isr` / `adc_isr` / `app_loop` steps against a
  simulated PMSM plant. This is **graph mode**: what runs is the code your
  graph compiles to, nothing more.
- **HostSIL** (`Images/HostSIL/`) — a software-in-the-loop harness that
  compiles the **unmodified Gen6FW application code** (Control, Calibration,
  Command, drivers, …) behind an STM32 HAL shim and runs it against the same
  ODE plant. This is **real-firmware mode**: the firmware's own boot
  sequence, control supervisor, and shell commands execute, on a simulated
  clock.

Both are pure host executables — no hardware, no cross-toolchain required.

## Which one should I use?

| | HostSim | HostSIL |
|---|---|---|
| What executes | Graph-generated domain code only | The real Gen6FW application, unmodified |
| Base image source | `Images/HostSim` | Emit of `Images/Gen6FW` (+ graph) behind `sil/` shims |
| Plants | ODE PMSM/induction (default), experimental ngspice | ODE PMSM/induction |
| Live GUI attach | Yes (`--live`; the sim publishes IVP over TCP `127.0.0.1:14608`) | Yes (`--live`; relays the firmware's own USART3 telemetry over TCP) |
| Best for | Iterating on graph control logic, demos, live tuning, GUI work | Firmware-level checks: boot, command handling, ISR cadence, exact firmware behavior |

Rule of thumb: develop and tune the *graph* in HostSim; verify the *firmware*
(including anything HostSim's scheduler does not model) in HostSIL; then
flash hardware.

## Quickstart: `rte sim` (HostSim, graph mode)

The `rte` automation CLI performs emit → build → run in one step:

```bash
./build/bin/rte sim --graph Assets/Examples/foc_demo.json
./build/bin/rte sim --graph Images/HostSim/graphs/spwm_demo_graph.json \
                    --scenario Images/HostSim/scenarios/spwm_demo.json
```

Batch mode runs as fast as the host can (`--realtime 0` default). The trace
CSV lands in `build/hostsim_<name>_emitted_build/run/`. Full flag spec,
scenario-resolution rules (`--name`, `--scenario` fallback to the matching
scenario or `default_motor.json`), `--no-build`, and output formats are
documented in [automation-backend.md](automation-backend.md) — including the
`--live` mode described next.

## Quickstart: live simulation + RTE Studio

```bash
# terminal 1 — emits, builds, and hosts the sim; stays in the foreground
./build/bin/rte sim --graph Assets/Examples/foc_demo.json --live

# terminal 2 — RTE Studio attaches to the live telemetry stream
./build/bin/RTEStudio Assets/Examples/foc_demo.json --tcp 127.0.0.1:14608 --protocol ivp
```

`--live` makes `host_sim` publish InverterProtocol (COBS-framed) telemetry on
`127.0.0.1:14608` and ignore `duration_s` (runs until quit / Ctrl+C).
`--realtime F` sets wall-clock pacing (`1.0` = realtime, `0` = as fast as
possible). RTE Studio's Runtime tab reconnects automatically after a
disconnect, plots the built-in keys (`throttle_a/b`, `duty_u/v/w`, `i_a/b/c`,
`theta_e`, `omega_e`, `vdc_v`) plus any `platform_telemetry_log_f32` keys
from the graph, and its console box sends HostSim shell commands back over
the same socket:

```text
throttle a 0.5        # live throttle override (a|b, 0..1)
duty u 60             # live duty override (u|v|w, 0..100; "duty clear" releases)
speed 0.25            # slow motion; "speed turbo" = no pacing limit
pause / resume
clear                 # drop all overrides
quit
```

One bundled end-to-end script does emit + build + live sim + GUI together:

```bash
./Images/HostSim/scripts/run_spwm_live.sh            # Linux
powershell -File Images\HostSim\scripts\run_spwm_live.ps1   # Windows
```

## Quickstart: standalone HostSim (base image only)

`host_sim` also runs without `rte` — useful when working on the base image
itself. Build it in place and run a scenario (run from `Images/HostSim` so
scenario-relative paths such as `trace_csv` and ngspice netlists resolve):

```bash
cd Images/HostSim
cmake -S . -B build_linux
cmake --build build_linux -j
./build_linux/host_sim scenarios/default_motor.json
python3 scripts/plot_sim.py trace.csv   # needs matplotlib
```

CLI (`host_sim --help`):

```text
usage: host_sim [scenario.json] [--live] [--listen host:port] [--realtime N] [--telem-hz N]
```

The scenario may also be passed as `--scenario <file>`. See
[Images/HostSim/README.md](../Images/HostSim/README.md) for emit-and-run
scripts, the SPWM demo walkthrough, and Windows live-mode tuning notes.

## Quickstart: HostSIL (real-firmware mode)

```bash
cmake -S Images/HostSIL -B build/hostsil_build
cmake --build build/hostsil_build -j
cd Images/HostSIL && ../../build/hostsil_build/host_sil scenarios/sil_foc_demo.json --realtime 0
python3 scripts/validate_trace.py sil_foc_trace.csv --control-start-s 1.6 --iq-a 8
```

The configure step auto-emits the firmware tree (`build/hostsil_fw_src` =
copy of `Images/Gen6FW/` plus graph-generated domain code). Pick a different
graph with `-DSIL_GRAPH=<path>` and a fresh `-DSIL_FW_SRC`, or re-emit with
`Images/HostSIL/scripts/emit_firmware.sh [graph.json] [output-dir]`.
`scripts/validate_trace.py` checks the trace for NaN/inf, the i_a+i_b+i_c
zero-sum constraint, overcurrent bounds, and that speed/duty respond to
control.

HostSIL also has a live mode (run from `Images/HostSIL`):

```bash
../../build/hostsil_build/host_sil scenarios/sil_foc_demo.json --live
```

`--live` serves the firmware's **own USART3 telemetry byte stream** — the
COBS-framed InverterProtocol packets its Telemetry module would put on the
wire — verbatim on `127.0.0.1:14608` (override with `--port P`), and implies
`--realtime 1.0` unless passed explicitly. RTE Studio attaches exactly as
with HostSim (`--tcp 127.0.0.1:14608 --protocol ivp`); the firmware emits the
usual 100 Hz DATA frames plus periodic DEFINE re-announces, so the full key
table appears regardless of attach time. It is a one-way link: bytes a client
sends are logged, not forwarded to the firmware shell. For a headless decode
of the stream there is `Images/HostSIL/scripts/ivp_probe.py`.

HostSIL scenarios extend the HostSim scenario format (see below) with
`control` (`start`, `start_time_s`, `iq_a`, `id_a` — posted through the
firmware's own `CommandManager`), `firmware_config` (KV pairs seeded via
`config set/save`), and `fram_image` (file backing for the emulated F-RAM).
`Images/HostSIL/scenarios/sil_foc_salient.json` demonstrates the salient PMSM
(Ld≠Lq) with a forced negative `id_a`: the plant's reluctance torque term adds
~23 % speed at equal `iq_a` versus `id_a = 0`.
Architecture, ISR ordering, and fidelity notes:
[Images/HostSIL/README.md](../Images/HostSIL/README.md).

## HostSim scenario file reference

Scenarios are plain JSON passed to `host_sim` (`argv[1]` or `--scenario`).
Defaults are built in — a scenario only overrides the keys it names. The
parser matches keys by name anywhere in the file, so the shipped scenarios
nest `plant`/`pwm_scope` under `simulation` while flat placement is also
accepted. Bundled scenarios: `default_motor.json`, `spwm_demo.json`,
`svpwm_live.json`, `ngspice_rl_demo.json`, `ngspice_pmsm_demo.json`,
`salient_pmsm.json`, `induction_vhz.json`.

### `motor.*` — machine parameters

Both simulators run the same ODE plant (`Images/HostSim/src/motor_model.cpp`).
`machine` selects the model:

| Key | Default | Meaning |
|---|---|---|
| `machine` | `pmsm` | `pmsm` = salient-dq PMSM; `induction` = squirrel-cage induction (stationary αβ model, `src/induction_model.h`) |
| `rs_ohm` | 0.05 | Stator resistance |
| `pole_pairs` | 7 | Pole pairs |
| `inertia_kg_m2` | 1e-5 | Rotor inertia (applied to the electrical speed, the simulator's mechanics convention) |
| `friction_nm_per_rad_s` | 1e-4 | Viscous friction (same convention) |
| `vdc_v` | 48.0 | DC-link voltage |
| PMSM only: `ld_h` / `lq_h` | 1e-4 | d/q inductance — Ld≠Lq enables the `(Ld−Lq)·id·iq` reluctance torque term |
| PMSM only: `flux_wb` | 0.01 | PM flux linkage |
| Induction only: `rr_ohm` | 0.3 | Rotor resistance (referred) |
| Induction only: `lm_h` | 0.025 | Magnetizing inductance |
| Induction only: `lls_h` / `llr_h` | 0.002 | Stator/rotor leakage (Ls=Lm+Lls, Lr=Lm+Llr) |
| `name`, `comment` | — | Informational (e.g. where to paste calibrated values) |

HostSIL parses the same keys (`Images/HostSIL/src/scenario.cpp`); its
`machine` maps onto the shared ODE plant identically.

### `throttle_a` / `throttle_b` — stimulus profiles

| Key | Profile | Meaning |
|---|---|---|
| `type` | all | `constant` (default), `ramp`, or `step` |
| `value` | constant, step | Output level (for `step`: the level before the step) |
| `start`, `end`, `start_s`, `end_s` | ramp | Linear ramp from `start` to `end` over the window |
| `step_time_s`, `step_value` | step | Jump to `step_value` at `step_time_s` |

### `simulation.*`

| Key | Default | Meaning |
|---|---|---|
| `duration_s` | 1.0 | Batch run length (ignored in live mode) |
| `tim_isr_hz` / `adc_isr_hz` | 10000 | Fast domain tick rates |
| `app_loop_hz` | 1000 | Slow domain tick rate |
| `telem_hz` | 500 | Live telemetry publish rate |
| `realtime_factor` | 1.0 | Wall-clock pacing; 0 = as fast as possible |
| `live` | false | Long-running mode with TCP telemetry |
| `listen_host` / `listen_port` | 127.0.0.1 / 14608 | Telemetry endpoint |
| `trace_csv` | `trace.csv` | Trace output path (relative to the process CWD) |
| `demo_fallback` | false | Legacy open-loop SPWM synthesized by the scheduler when no graph node drives the duties; only `default_motor.json` enables it (plant bring-up without a graph). When off and duties stay 0 despite non-zero throttle, HostSim logs a warning once. |
| `config_file` | — | Backing file (`key=value` lines) for the `platform_config_*` store; preloaded at startup, flushed on every set. Absent = in-memory only. |
| `pwm_carrier_hz` | — | Shorthand: set the PWM scope carrier and enable the scope. |

### `simulation.plant` (or top-level `plant`) — backend selection

| Key | Default | Meaning |
|---|---|---|
| `backend` | `ode` | `ode` or `ngspice` |
| `netlist` | — | ngspice netlist path, relative to the `host_sim` working directory (bundled netlists live in `Images/HostSim/plants/`) |
| `substeps` | 4 | SPICE substeps per control tick (zero-order hold on phase voltages across the tick) |

### `simulation.pwm_scope` (or top-level `pwm_scope`) — switched-waveform scope

| Key | Default | Meaning |
|---|---|---|
| `enabled` | false | Publish `pwm_gate_*` / `pwm_v_*` scope channels |
| `carrier_hz` | 800 | Triangle carrier frequency |
| `telem_hz` | auto | Scope sample rate (scales with sim speed) |

### `adc.*` — phase-current ADC error model (absent = ideal)

Defaults mirror the Gen6 signal-chain constants in
`Images/HostSim/include/RteParams.h`.

| Key | Default | Meaning |
|---|---|---|
| `resolution_bits` | 16 | ADC resolution |
| `vref_v` | 3.3 | ADC reference voltage |
| `ref_v` | 1.65 | Zero-current sense offset |
| `divider` | 2/3 | Sense-chain divider |
| `sensitivity_v_per_a` | 1.042e-3 | Amps-to-volts at the ADC input |
| `gain_error` | 1.0 | Multiplicative gain error |
| `offset_u_a` / `offset_v_a` | 0 | Additive current bias per sampled phase |
| `noise_std_a` | 0 | Gaussian noise, 1-sigma amps |

### `environment.*`

| Key | Default | Meaning |
|---|---|---|
| `motor_temp_c` | 25 | Surfaced by the platform temperature APIs |
| `inverter_temp_c` | 25 | Surfaced by the platform temperature APIs |

### `faults.*` — triggers surfaced via `platform_has_critical_fault()`

| Key | Default | Meaning |
|---|---|---|
| `overcurrent_a` | 0 (off) | Trip when any &#124;i_phase&#124; exceeds this |
| `undervoltage_v` | 0 (off) | Trip when the DC link drops below this |
| `vdc_glitch_time_s` / `vdc_glitch_v` | off | At the given time, drop the DC link to `vdc_glitch_v` (seen by both control code and plant) |

### `can.*`

| Key | Default | Meaning |
|---|---|---|
| `loopback` | true | Frames sent via `platform_can_send` are readable via `platform_can_rx` (latest-frame store keyed by bus+id) |
| `frames` | — | Scheduled injected traffic: `[{"bus": 1, "id": 291, "ext": false, "start_s": 0.1, "period_s": 0.01, "data": "DEADBEEF"}]`. `id` accepts decimal or `0x` hex, `data` is a hex string ≤ 8 bytes (`dlc` follows), `period_s` > 0 repeats otherwise single shot at `start_s` (alias `time_s`). |

### `vars.*` — graph Var node seeds (HostSim only)

`{"vars": {"TargetHz": 40.0}}` writes each value into the `Stored` state of
the emitted graph's Var node with that id, right after domain init — the
batch-mode equivalent of the firmware shell's `var set` used by
`Tools/alternate_target_hz.py`-style live sessions. Unknown names are warned
about and ignored (base-image-only runs have no graph and warn once).
`scenarios/induction_vhz.json` uses this to set the induction_vhz example
graph's frequency target; Values.Config nodes are instead seeded from the
`simulation.config_file` KV store (e.g. `scenarios/induction_vhz.cfg` for the
V/Hz ratio).

Motor parameters are not hardcoded to a machine: copy a bundled scenario and
paste calibrated values from the target motor.

## Plant backends

| Backend | Scenario | What it models |
|---|---|---|
| **ODE** (default) | `"backend": "ode"` or omitted | Discrete-time machine ODE (`src/motor_model.cpp` + `src/induction_model.h`): clamped duty × Vdc drive → averaged phase voltages → plant dynamics; integrated mechanics. `motor.machine` picks `pmsm` (salient dq, reluctance torque included) or `induction` (4th-order squirrel-cage, stationary αβ, rotor-flux angle/slip internal). Fast — the only fully supported backend and the right choice for `--live`. |
| **ngspice RL** (experimental) | `"backend": "ngspice"`, `"netlist": "plants/inverter_rl.cir"` | Three-phase wye RL load in libngspice. The circuit has no back-EMF element, so the back-EMF is folded into the driven source values; mechanics integrate in the host. |
| **ngspice PMSM** (experimental) | `"backend": "ngspice"`, `"netlist": "plants/inverter_pmsm.cir"` | Per-phase R-L plus an **in-circuit back-EMF source** (`Veu/Vev/Vew` external sources driven from rotor angle/speed), so back-EMF is part of the circuit equation. |

Both ngspice netlists are voltage-source driven (duty → phase terminal
voltage, vs DC−), with netlist components parameterized from `motor.*`
(`alterparam`, cold start via `UIC`). ngspice models **electrical RL /
PMSM-backEMF only — no induction machine**; `machine: "induction"` with
`backend: "ngspice"` is refused with a stderr notice and continues on the ODE
plant (which models the induction machine natively). If `libngspice` cannot
be loaded, HostSim prints a notice on stderr and continues on the ODE plant —
check the log when a scenario unexpectedly runs ODE.

## ngspice backend setup (Linux, optional)

The backend `dlopen`s `libngspice.so` first, then `libngspice.so.0`, so only
the shared library is needed at runtime — no headers, no rebuild:

```bash
sudo apt install libngspice0-dev    # provides libngspice.so (linker name)
```

The runtime-only package also works: `libngspice0` ships the versioned
`libngspice.so.0`, which the loader finds on the standard library path. If
your library lives somewhere non-standard, point the loader at it:

```bash
LD_LIBRARY_PATH=/path/to/dir-with-libngspice ./build_linux/host_sim scenarios/ngspice_rl_demo.json
```

Then run a demo **from `Images/HostSim`** (netlist paths are relative to the
process working directory — `rte sim` runs `host_sim` from its own run
directory, so launch ngspice scenarios directly):

```bash
cd Images/HostSim
./build_linux/host_sim scenarios/ngspice_rl_demo.json
./build_linux/host_sim scenarios/ngspice_pmsm_demo.json
```

Environment knobs:

- `HOSTSIM_NGSPICE_SYNC_JUMP=1` — experimental performance opt-in: the sync
  callback jumps straight to each stop target instead of re-ramping the
  timestep after every pause (~1.8× fewer internal SPICE steps; only a few
  percent wall-time on the bundled demos, with slightly different
  trajectories). Default off.
- `HOSTSIM_TELEM_STDERR=1` — also echo `platform_telemetry_log_f32` traffic to
  stderr (off by default; debugging only).

## Known limitations / fidelity caveats

**Both simulators**

- Averaged-duty drive: no switching ripple, no dead-time/distortion effects.
  HostSim's `pwm_scope` is a visualization layer, not the plant.
- Sensors are ideal apart from the modeled quantization/offset/noise terms.
- Not a replacement for dyno validation — treat results as
  control-logic/plant-model truth, not hardware truth.

**HostSim**

- The ngspice backend is experimental: voltage-source-driven (no switched
  devices), limited to the RL/PMSM netlist shapes above, slower than the ODE
  plant, and it falls back to ODE when libngspice is missing.
- Number parsing is a lenient key search, not a strict JSON DOM — malformed
  files can silently keep defaults.

**HostSIL** (condensed from its README — that file is authoritative)

- Cooperative scheduling: ISRs run at tick boundaries and never preempt
  mid-instruction; firmware critical sections model a *stronger* guarantee
  than hardware.
- One perfectly clean injected ADC burst per switching period; TIM1-OC4
  adaptive trigger placement is bookkeeping-only.
- CAN/UART frames are accepted and dropped; telemetry TX DMA completes at the
  next app tick (in `--live` mode the USART3 TX bytes are also relayed to TCP
  clients); F-RAM is a 256 KiB in-memory image (optional file backing).
- `platform_micros()`/DWT see cycles = sim µs × 550 MHz.

## Future directions

Deliberately not built yet, but the seams are in place (see also the TODO.txt
simulator block and the root README roadmap):

- **Synchronous DC/DC and DC microgrids.** One 3-phase stage driven as three
  independent phase→DC-bus converters, a 2+1 split, or all legs paralleled.
  The work item is a DC-bus/load plant behind the existing `IPlant` seam
  (`src/plant/plant_backend.h`); an ngspice netlist can already express a sync
  buck/boost of arbitrary shape, so a netlist-only prototype works today.
- **Multiple inverters at once.** Two instances (e.g. a 5-phase motor driven
  by two 3-phase inverters, or a microgrid AFE → DC/DC → output chain) need a
  shared simulated CAN: HostSim already models loopback CAN per instance; the
  bridge is a small socket relay between instances. Sim instances already take
  distinct ports (`host_sim --listen`, `host_sil --port`) so N RTEStudio
  sessions can attach independently.
- **N-phase machines.** The induction/PMSM models are 3-phase dq/αβ; a
  5-phase machine means generalizing the plant transforms behind the same
  seam.

## Design history

The ngspice backend was delivered against
[Images/HostSim/docs/Implementation_Plan.md](../Images/HostSim/docs/Implementation_Plan.md).
That document is the historical design plan: the phases it describes (the
`IPlant` seam in `src/plant/plant_backend.h`, the ODE refactor, the
sharedspice loader, scenario-selected backends, live IVP telemetry) have
landed, but its status tables predate the implementation — read it for
rationale, not for current status.

## Further reading

- [Images/HostSim/README.md](../Images/HostSim/README.md) — base-image
  architecture, emit-and-run scripts, SPWM demo, Windows live tuning
- [Images/HostSIL/README.md](../Images/HostSIL/README.md) — SIL architecture,
  hardware-order ISR walkthrough, full fidelity notes
- [automation-backend.md](automation-backend.md) — full `rte sim` / CLI
  contract, cache layout, MCP
- [Source/NodeGUI/README.md](../Source/NodeGUI/README.md) — RTE Studio live
  attach, Runtime tab, presets
- [Images/HostSim/docs/Implementation_Plan.md](../Images/HostSim/docs/Implementation_Plan.md)
  — original ngspice integration plan (historical)
