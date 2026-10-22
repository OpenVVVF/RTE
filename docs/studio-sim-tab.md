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
- **Boot console quotes the seeded gains.** The `[FOC] Kp set to …`,
  `[FOC] Ki set to …` and `[CFG] motor config … kp=/ki= …` lines quote the
  `Ctrl.PiD.Kp/Ki` / `Ctrl.PiQ.Kp/Ki` FRAM/graph seeds the generated
  controller loads at domain init (d/q axes shown as `d / q`), not the
  recorded firmware's 0.0300/10.0000 literals — editing a `Values.Config`
  DefaultValue between runs changes the boot lines to match (P1-r2-4).
- **Duration spin clamp is no longer silent.** Typed entries above the
  300.00 s Studio limit log a `[warn] duration … clamps …` row naming the
  clamp and the CLI alternative (`rte sim --duration <s> --out …` has no
  session-length cap) at edit time, again as a Run-transcript row if a run
  starts with a clamp pending, and on an amber badge next to the spin.
- **Locked-rotor-by-default gets a warning row.** A run with no mechanical
  source — Dyno rpm and series fields empty, no `dyno-set` event, and the
  plant graph's `Dyno.Rpm0 == 0` (every shipped bench rig ships 0) — adds a
  `⚠ mechanical speed static at 0 (Dyno rpm source unset) — locked-rotor
  bench` warn row to the run log/progress strip and a `⚠ locked-rotor
  bench` chip on the Complete status line. The green strip alone no longer
  has to teach "the bike never turned once".
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

### Telemetry cursor + pan (shipped)

Both telemetry surfaces (this tab's Telemetry view and the Runtime tab) share
the same plot-leaf behavior; each surface keeps its own cursor.

- **Left-drag on any plot** pins a vertical time cursor, mirrored at the same
  data time in every plot of the surface. Release keeps it pinned; a second
  drag moves it; **double-click clears it** and snaps the window back to the
  newest edge.
- **The signal table follows the cursor**: while pinned, the Value column
  shows each channel's value interpolated at the cursor time
  (`runtime::TelemetryStore::ValueAt` — the sim side reads through the same
  retained store the plots draw from, so any point of a recorded session is
  reachable, not just the tail); with no cursor it shows last/latest as
  before. The sim table's header flips "Last" -> "Cursor" as a reminder.
- **The mouse wheel over a plot pans the window** off the newest tail
  (up = toward newer, step = view/4 per notch); wheeling back onto the newest
  edge re-engages tail-following automatically. Scrolling the plots column
  itself uses the scrollbar — the wheel over a plot belongs to the plot.
- On the **live Runtime tab** a pinned cursor additionally **freezes the
  window**: the stream keeps running behind the hold, forward pans clamp at
  the newest sample without re-following, and double-click is the release
  that finds the tail again.
- Cursor state is **not persisted** anywhere (only the graph assignments
  are); a newly loaded session always starts tail-following.
- Offscreen smoke has read dumps for all of this: `simCursorInfo` /
  `rtCursorInfo` labels (`cursor <t> · window <a..b> s · following|held|panned`)
  plus the signal-table dumps; gesture pokes: `dragcursor`, `clearcursor`,
  `panwindow`, and synthesized-input `plotdrag` / `plotdblclick` / `plotwheel`.

**View-slider semantics change:** the Telemetry view's "Plot view (sec)"
slider no longer caps at a hard 60 s - its maximum is sampled from the loaded
session's span, so e.g. a 3685 s recording can be widened into one window
(proven on `twin/out/im_noload_1h_sim.jsonl`). The Runtime tab slider and its
0.5..60 s range are unchanged, as is the initial view after a load
(min(duration, 60 s)).

### Recorded-session overlay (shipped, backlog P1-8)

Both telemetry surfaces (this tab's Telemetry view and the Runtime tab's
telemetry panel) can overlay one recorded v3 session on top of whatever is
showing - another loaded session or the live stream.

- **"Overlay: Load…"** parses the reference file through
  `sim::SimSessionSeriesSource` (the dossier's design-B seam) into an overlay
  set in the surface's `runtime::TelemetryStore` (`SetOverlayChannels` et al.
  - additive store API only; the live ring and `ClearSession` semantics are
  untouched). The info label (`simOverlayInfo` / `rtOverlayInfo`,
  smoke-dumpable) reads `overlay <stem> · N ch · D s · offset ±x s`.
- **Per-signal opt-in is the existing graph assignment**: the reference trace
  renders only for signals the user assigned to a plot, drawn in the same
  QPainter pass as the cursor - dashed, thinner, desaturated against the
  same palette color. Autorange includes the overlay so it never clips.
- **Per-overlay time offset** (numeric spin + slider, seconds / centiseconds;
  slider range widens to the overlay span on load): a lookup at axis time T
  samples overlay time T − offset, so a positive offset shifts the reference
  toward newer time for phase-locked comparison.
- **Delta readout at cursor**: while a cursor is pinned, the sim value table
  shows `live (ref R, Δ ±D)` on rows whose channel the overlay carries (ref
  sampled under the current offset through the same interpolated lookup);
  channels not in the overlay keep the plain wave-5 value cell. The runtime
  Value column is unchanged this wave (no delta column there).
- **Overlay state is never persisted** (same convention as the cursor) - BUT
  it survives loading a different primary session in the sim view, which is
  exactly the run-N-1-vs-run-N comparison flow: load run N-1 as overlay, run
  / load run N as primary, look.
- Offscreen smoke verbs: `overlayload` / `overlayoffset` / `overlayclear`
  (`sim|runtime`), with the `simOverlayInfo` / `rtOverlayInfo` label dumps
  and the offset slider dumps for read-back.
- Deferred garnish (deliberately not this wave): multiple overlay sets,
  per-series styling UI, auto cross-correlation alignment, and the
  `twin.metrics` embedding from "Diff vs recorded" below.

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

**Fault markers are honest, not emulated.** Plant-side trip physics
(gate_ready fall, MOE drop, open-gate freewheel collapse, encoder-loss
telemetry freeze) genuinely runs on the generated plant, and the
`[FAULT]`/`[SUP]` console vocabulary around scripted `reset`/`start` events
is deliberate host choreography mirroring the recorded boot vocabulary.
What does NOT run in the sim host is the **emitted fault manager itself**:
F02 verified that FaultManager/ControlSupervisor are not among the compiled
units (the ADC watchdog, TIM1 break, gate-driver /RDY/UVLO and MAX22530
paths they'd need are absent too), so `fault_flags_hex` and
`fault_active_names` can never move off `0x00000000` / `[]`. A forever-zero
word is NOT a protection verdict: the session says so in-band with a boot
console line (`[FAULT] sim host: no fault manager linked — …`) and a
`fault_manager_emulation=manager_absent_sim_host` t=0 state stamp, while
the flag values keep their exact-string shape for twin tooling's
exact-match checks. The open verification item remains **F02**: linking the
real protection chain (FRAM store first, per the host header note), after
which a real-firmware trip diff against the recorded triple-redundancy
trips becomes meaningful (glossary semantics in the contract §Host
interface).

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
