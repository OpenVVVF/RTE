# Design note + shipped behavior — RTE Studio "Simulate" tab

Status: **landed** (the Simulate tab ships in RTEStudio; this note keeps the
original design intent first and then pins what the shipped screen does).
Backend: `rte sim` (closed-loop, landed). Plant graph editing ships as a
separate "Plant Editor" tab with its own GraphScene/clipboard/history on
`Assets/PlantTemplates`. Background: `docs/plant-nodegraph.md` (the plant
side of the loop), `docs/automation-backend.md` (Studio = thin frontend over
the portable `rte` executable).

## Idea in one sentence

One tab where the controller graph and the plant graph sit side by side on the
same canvas component, a run bar drives a headless `rte sim` through the
existing automation session, and the resulting v3 JSONL session renders as
overlay charts with fault injection and diff-vs-recorded one click away.

### Shipped form (verified in headless grabs)

Two separate tabs carry this today: the **Plant Editor** (its own document
and a single `plant` domain; interactive validation skips the controller
cycle rule and rejects bridges) and **Simulate** (two path rows with project
defaults, a scenario row - duration/dt-sub/dyno-rpm|series/supply/events -
with an openly regex-level live validator, Run/Cancel on a QProcess driving
`rte --format jsonl sim`, progress stream mirrored in the log pane, artifact
auto-load into a Telemetry view with G1/G2/G3 assignment and the GL plots
from `runtime/SignalPlotWidget`). This differs from the original "two views
of one canvas" sketch only in packaging: the two documents have their own
editors rather than sharing one canvas. What is NOT landed: diff-vs-recorded
overlays (twin.metrics embed), fault injection presets as one-click buttons
(the events editor carries them by name pattern), and cancel-of-in-flight
scout runs from the progress list. On the headless offscreen test platform
`QOpenGLWidget is not supported` leaves the plot canvases blank; the
structure and the table still render - see `Source/NodeGUI/src/sim/` for
the implementation.

### Live-behavior caveats (dogfood-proven)

- **FRAM config policy:** graph config nodes (`config.*` / `Values.Config`
  with `Key`/`DefaultValue`) seed the scenario FRAM: a value the graph names
  wins over the twin-calibrated baselines, exactly like hardware FRAM-create
  semantics - this is what makes tuning e.g. `Ctrl.Iq.SlewAps` from the GUI
  actually move the ramp (verified 25.4 -> 2.9 A/s for DefaultValue 50->5).
  Keys the graph never names keep the twin-replica defaults so recorded
  campaign parity is untouched.
- **`control start` resets runtime vars to graph defaults** (the emitted
  code's Start() runs at every control start). A scripted ramp must re-issue
  `var-set` events after each `start` - references don't survive a stop on
  real firmware either.
- **Excluded nodes keep their telemetry channels**, flatlined at 0 (the
  session vocabulary is channel-stable across variants so diffs line up;
  the real firmware emits fewer keys there too).
- **`tw_p_*` are the fitted-law diagnostics** - they respond to current,
  speed and bus voltage by the calibrated v2 law, not to knee/IGBT params
  (deadtime/knee affect the voltage-side shaping only, which the law is
  not parametric in - by design of the calibration evidence).
- **CAN path in the sim is a stub** (`platform_can_*` no-ops): CAN-FD
  drive-control emulation is the twin ROADMAP M6 item; the living CAN path
  today is FD hardware + SocketCAN + `rte trace record`.

## Layout

- **Two views of one canvas.** The existing graph canvas is reused twice —
  once bound to the controller graph (domain timing/bridges, the graph being
  "the project"), once bound to the plant graph (`Assets/PlantTemplates`,
  single `plant` domain). Editing is identical (nodes, wires, float-string
  parameters); the only differences are the template palettes and the targets
  shown in the view chrome ("compiles to firmware" vs "compiles to simulator
  step"). No new editor component is designed here.
- **A run bar** along the top: controller graph selector (default: the open
  project), plant graph selector (default: the matching one from
  `Assets/PlantExamples/`), base source (`Images/Gen6FW`), duration, dyno
  rpm, working-directory/out file, and **Run**. The bar does not build
  anything itself — it composes the command line

  ```bash
  rte sim --sw-graph <controller.json> --plant-graph <plant.json> \
          --base-source Images/Gen6FW --duration S \
          --dyno-rpm R | --dyno-series 't=rpm,t=rpm,...' \
          --dt-sub 0.0002 --telemetry-hz 100 \
          --supply 'v_set=..,i_limit=..,sink_limit=..,can_sink=0|1,cv_clamp_v=..,cv_clamp_sink_a=..,series_r_mohm=..' \
          --event 't=.. kind=.. arg=..' --out session.jsonl
  ```

  and launches it the way Studio launches every automation job today: an
  argument array against the discovered RTEAutomation session. The
  **`--format jsonl` streaming progress contract is real**: phase progress,
  artifact, and completion events stream back as jsonl for the progress strip
  (the same Emit machinery `rte build` uses), and cancelling a run uses the
  same job lifecycle as `rte build`/`flash`.
- **Charts** below the canvas: the written session
  (`rte-runtime-session-jsonl` **v3**, same schema the twin emits, so
  simulated and recorded sessions align key-for-key) loads into the existing
  telemetry charting: channel picker, multi-channel overlay, cursor with
  per-channel values. Node-level truth channels (e.g. machine id/iq, DC-link
  power) are just more chart series. `session_start` carries a plant
  **provenance stamp** (`"plant": "generated" | "fallback"`) so a run made
  against the built-in fallback plant can never be confused with a
  graph-generated plant run — and the fallback is now opt-in anyway
  (`--allow-fallback-plant`; the default build refuses to substitute). The
  UI surfaces the toggle and the stamp rather than hiding them.
- **Substep knob.** `rte sim` exposes the plant substep (`--dt-sub`, default
  200 µs, 2 substeps per control period). The shipped, proof-covered IM
  configuration (`teco_vhz_induction.json`) is validated at 50 µs — the stiff
  TECO rotor modes must not be presented at the default 200 µs without a
  warning in the UI chrome.

## Scenario events

The run bar's event list is a small grid of rows (time, kind, argument) that
serializes straight into repeated `--event 't=.. kind=.. arg=..'` flags. The
kinds map onto the plant contract's host-driven externals (the python/C++
spellings are in `docs/plant-nodegraph.md`'s glossary table):

- `reset` → supervisor fault-reset choreography (fault flags clear + gate
  reset), `start` / `stop` → control on/off (duties live vs the 50/50/50
  zero-vector boot default).
- `var-set` → writes a controller runtime variable (`arg=Name=Value` — the
  `g_<domain>_vars` tables; the twin's `var` event kind).
- `fault-inject` → `PlantSetFault(plant, name, true)` on the graph: a
  **physics-level** named plant fault (`OpenGates`, `EncoderDisconnect`,
  `SensorRail` — the `Plant.Fault.Point` port vocabulary), addressed by
  output port name. The C++ accessor returns `bool`, so a typo'd name fails
  loudly instead of pretending to inject. `fault-clear` clears one or all
  (`arg=<Name>|empty`).
- `dyno-set` → `PlantSetDynoRpm`, republishing the derived speed views
  (`Omega_Elec`/`Rpm`/`Elec_Rpm`).
- `gate-ready` → the gate-driver-ready power transition (the recorded
  gatekill class: drives the plant physics behind `gate_ready` with its
  console/state vocabulary).
- `plant-input` → `PlantSetInput(plant, name, value)`: one-shot assignment of
  **every unwired plant input of that name** — the controller-side externals
  (`arg=Omega_Cmd=<rad/s>` for `InductDq`, the IM's synchronous speed; the
  V/Hz controller pushes it per control period). Together with
  `induction_vhz.json` + `teco_vhz_induction.json` this makes a V/Hz IM run
  expressible from the CLI; a scripted ramp is the `var_series` follow-up
  (not yet on the timeline, with `config set` and `reboot`).

The DC/DC converter rigs drive their externals through the same
`kind=plant-input` event — those graphs carry no `DutyIn`/`Dyno`, so duty
(BuckBoost2Q) or bridge phase shift (DualActiveBridge) is a named unwired
input scriptable on the timeline:

```bash
rte sim --plant-graph Assets/PlantExamples/buckboost_charger_example.json \
        ... --event 't=0.5 kind=plant-input arg=Duty=55'
rte sim --plant-graph Assets/PlantExamples/dab_pack_example.json \
        ... --event 't=0.5 kind=plant-input arg=PhaseShift=0.05'
```

Both converter packs are **provisional**: every constant is a stand-in
calibration target with no recorded evidence yet (what is proven today is the
dual-polarity converter semantics, per the graphs' description strings and
`twin/plantgraph/proof_machines_parity.py`).

Nothing is scheduled inside Studio: the runner applies events at sim time;
Studio only edits a declarative timeline.

**Fault markers are real.** Trip-side behavior (gate_ready fall, MOE drop,
fault flags, console `[FAULT]`/`[SUP]`/`[SAFETY]` vocabulary, graph-telemetry
freeze) comes from the **actual emitted fault manager running in the loop**,
not a host-side fake — so a fault window in a sim session differs from a
recorded window only where the protection physics does. The open verification
item here is **F02**: an end-to-end diff of a real-firmware-protection fault
run against the recorded triple-redundancy trips (harness physics-vs-
protection glossary semantics apply, see the contract §Host interface).

## Diff vs recorded

Because the session schema is the twin's v3, any recording from
`Documentation/Docs/Safety-and-Compliance/Testing/` (loaded through the
twin's replay loader) is a drop-in comparison target. The chart view gets a
"compare against…" action that embeds what `twin/metrics.py` already
computes headless — `compare_sessions(sim_path, rec_path, spec)` produces
per-channel bias/RMSE windows and `report_text()` the narrative — rendered
as per-channel verdict badges plus overlay pairs (sim channel vs recorded
channel on one axis with the tolerance band shaded). Fault runs compare on
fault-bit timelines (`fault_markers`), matching how the twin's own
experiments are judged. The tab stays a *viewer* of metric results; the
metric engine remains python-side.

## Non-goals (for v1)

No live/interactive stepping, no rewiring mid-run, no SPICE-level switch
transients (the sub-cycle fault class belongs to the M6 ngspice path), no
editing of protection behavior — protection lives in the base image around
the controller graph, exactly as on hardware.
