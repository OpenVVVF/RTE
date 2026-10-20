# Plant Templates — node library for OPEN-VVVF *plant* nodegraphs

This directory holds the **plant-side** node library used by plant nodegraphs
(HW side of the loop), sibling to `Assets/NodeTemplates` (SW/controller side).
The point of the split: the controller graph and the plant graph share the
same node format, but their target is different - the controller graph is code-
generated into firmware; the plant graph is generated into a simulator step
function and is run by `rte sim` (headless) or RTE Studio (UI) in closed loop
with the controller build.

## A template folder

Same layout as controller templates:

```
PlantTemplates/<Node.Id>/
  node.json      # ports, parameters, description, isEntryPoint
  inline.cpp     # the per-substep evaluation body
  constructor.cpp # (optional) init body, run once before the first substep
```

## Execution semantics (the plant contract)

- A plant graph executes one implicit domain `plant` (no timing-domain split,
  no bridges - disallow them for now; emitter errors on any `bridges` entry).
- **One evaluation pass per integration substep** (dt handed by the host sim,
  typically 25-200 us). The validated evaluator semantics — proven against the
  certified monolithic plant (`twin/plantgraph/proof_bench_parity.py`, overall
  PASS, encoder counts bit-exact, OpenGates collapse zero-divergence) — are:
  1. Host nodes emit (`Plant.Io.DutyIn`, `Plant.Shaft.Dyno`,
     `Plant.Fault.Point`): the host assigns their outputs out-of-band.
  2. Scheduling loop: a **combinational** node becomes ready once every
     stateful node in its ancestor cone has integrated this substep, AND its
     combinational ancestors have evaluated. A **stateful** node becomes
     ready once its combinational ancestors evaluated and its stateful
     ancestors integrated. Ready nodes evaluate immediately (declaration
     order breaks ties).
  3. Cycle-lock rule: nodes that close a loop back into a stateful node
     (e.g. `Knee3ph` reading machine currents that are only integrated later
     in the same substep) never unblock; when the loop stalls, the pending
     stateful node with the fewest pending stateful ancestors is forced: its
     combinational ancestors evaluate against **PRE** values (producer
     outputs from the previous substep, i.e. stored state), then the node
     integrates. Repeat until everything evaluated.
  4. Reading rule: a consumer reads the producer's output value *as of the
     moment it evaluates* — POST (post-integration) if the producer already
     integrated this substep, PRE otherwise. This reproduces the monolithic
     twin's imperative substep order exactly for every graph built from this
     library.
- Stateful integrators are written as plain Euler updates in the node body
  (`X += dX_dt * dt;`) — compute ALL derivatives from the pre-update state
  into temporaries first, then assign (the reference parity found this the
  hard way: assigning `id` before computing `diq` breaks bit-comparability).
  PHYSICAL RAILS: any node whose state can escape numerically must clamp in
  its own body (reference: twin/plant).
- Reserved externals: nodes `Plant.Io.DutyIn` (du/dv/dw from the controller
  shim), `Plant.Io.MeasOut` (phase currents / vdc / encoder counts / temps /
  dyno speed collected into a `PlantMeas` struct the host reads per step).
  `isEntryPoint: true` marks the do-nothing pass-through that the emitter
  uses to bind externals (universally-scoped names are the hardware seam).
- The emitter generates from the graph:
  - `struct PlantState` (one field per persistent output/state per node),
  - `void PlantInit(PlantState& state)`  (constructor bodies + defaults),
  - `void PlantStep(PlantState& state, double dt)`  (nodes emitted in the
    statically-computed schedule above; the schedule is order-deterministic
    per graph, so the emitter freezes it at codegen time).
- Unconnected optional inputs zero-bind. `Plant.Fault.Point` outputs are
  raised by the host by name (`set_fault`), default 0.

## Parameters / namespacing

- `parameters` in the graph node instance are the same strings used by the
  controller graphs (floats written as strings).
- Template namespace: `Plant.<Group>.<Name>` with groups: `Io`, `Inverter`,
  `Machine`, `Shaft`, `DcBus`, `Loss`, `Thermal`, `Sensor`, `Fault`, `Util`.
  Keep names short, singular, case-same as the twin's python.

## Current library (v1)

| Node | Semantics |
|---|---|
| Plant.Io.DutyIn | du/dv/dw external duty inputs (host-assigned) |
| Plant.Io.MeasOut | measurement collector (PlantMeas seam) |
| Plant.Inverter.Knee3ph | duty+vdc -> phase volts w/ IGBT knee + deadtime + Rce |
| Plant.Machine.PmsmDq | PMSM dq-Euler, torque, angle, abc currents, open-gate collapse |
| Plant.Machine.InductDq | induction machine, stationary-frame coupled-L (V/Hz) |
| Plant.Shaft.Dyno | external rotor-speed input (host-assigned) |
| Plant.Shaft.FreeInertia | J/B shaft integration from machine torque |
| Plant.DcBus | link capacitor + bench supply (CV/source/sink/CV-clamp) |
| Plant.Loss.FittedLaw | v2 total-loss law + zero floor + plate share + idle heat |
| Plant.Thermal.TwoNodeCauer | plate -> heatsink -> ambient + NTC spot offsets |
| Plant.Sensor.Hall | quantize + clamp + isolated-rail phantom mode |
| Plant.Sensor.VoltageSense | first-order lag + 0.44 V LSB quantization |
| Plant.Sensor.SinCosEncoder | theta -> bounded counts, disconnect rails |
| Plant.Fault.Point | host-raised named fault flags (OpenGates, EncoderDisconnect, SensorRail) |
| Plant.Util.Sum | Out = A+B+C (C optional) |
| Plant.Util.Hypot | Out = hypot(A, B) |
| Plant.Util.AbsOmega | Om = abs(Omega_Elec)/Poles |

Authoritative per-node parameter lists are in each folder's `node.json`.
Physics sources of truth are the python reference (`twin/plant/plant.py`,
`twin/plant/induction.py`) and the recorded campaign
(Documentation/.../Testing). New templates: EXACT same structure; keep
semantics identical so card runs stay green regardless of which engine
(python or C++ plant) runs the graph.
