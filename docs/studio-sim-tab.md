# Design note — RTE Studio "Simulate" tab

Status: design intent only (no Qt code yet). This note pins the UX and the
seams so the tab can land on top of `rte sim` (in-flight) without inventing
any new backend mechanism. Background: `docs/plant-nodegraph.md` (the plant
side of the loop) and `docs/automation-backend.md` (Studio = thin frontend
over the portable `rte` executable).

## Idea in one sentence

One tab where the controller graph and the plant graph sit side by side on the
same canvas component, a run bar drives a headless `rte sim` through the
existing automation session, and the resulting v3 JSONL session renders as
overlay charts with fault injection and diff-vs-recorded one click away.

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
          --base-source Images/Gen6FW --duration S --dyno-rpm R \
          --event 't=.. kind=.. arg=..' --out session.jsonl
  ```

  and launches it the way Studio launches every automation job today: an
  argument array against the discovered RTEAutomation session, `--format
  jsonl` progress events streamed back for a progress strip. Cancelling a
  run uses the same job lifecycle as `rte build`/`flash`.
- **Charts** below the canvas: the written session
  (`rte-runtime-session-jsonl` **v3**, same schema the twin emits, so
  simulated and recorded sessions align key-for-key) loads into the existing
  telemetry charting: channel picker, multi-channel overlay, cursor with
  per-channel values. Node-level truth channels (e.g. machine id/iq, DC-link
  power) are just more chart series.

## Fault injection

The run bar's event list is a small grid of rows (time, kind, argument) that
serializes straight into repeated `--event` flags. The kinds map 1:1 onto the
plant contract's host-driven inputs:

- `fault` → a named `Plant.Fault.Point` setter (`OpenGates`,
  `EncoderDisconnect`, `SensorRail`, ...), i.e. what the python reference
  calls `sim.set_fault(name, value)`. UI offers the known names from
  `Plant.Fault.Point/node.json` plus free text.
- `dyno` → `Plant.Shaft.Dyno` speed reassignment.
- (later) duty/throttle step rows map onto the remaining host seam.

Nothing is scheduled inside Studio: the runner applies events at sim time;
Studio only edits a declarative timeline.

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
