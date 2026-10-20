# Plant nodegraphs — the HW side of the OPEN-VVVF co-simulation

This is the canonical description of the *plant* nodegraph: what it is, how it
executes, what ships in the v1 library, and how it is validated. For the
binding wording of the execution contract itself see
`Assets/PlantTemplates/README.md` (§Execution semantics); this document
explains the why and the worked details around it.

## Why the plant is a nodegraph

The controller you develop in RTE is a nodegraph that gets code-generated into
a firmware image. The plant — inverter, machine, DC bus, sensors, thermals —
is a nodegraph too, with the **same file format but a different target**:

- A **controller graph** (`Assets/NodeTemplates`, domain-timed, with bridges)
  compiles through InverterCodegen into firmware C++.
- A **plant graph** (`Assets/PlantTemplates`, single implicit `plant` domain,
  no bridges — the emitter errors on any `bridges` entry) compiles into a
  **simulator step function** that runs headless under `rte sim` (or inside
  RTE Studio) in closed loop with the controller build.

One graph format, one editing canvas, one parameter-string convention; two
targets. The SW and HW sides of a drive become co-versionable artifacts, and a
controller graph can be exercised against its future hardware before any
hardware exists.

A template folder is exactly the controller-template layout the NodeAPI loader
already understands (`NodeAPI::LoadNodeTypesFromDirectory`):

```
PlantTemplates/<Node.Id>/
  node.json        # ports, parameters, description, isEntryPoint
  inline.cpp       # per-substep evaluation body
  constructor.cpp  # optional init body, run once before the first substep
```

Template bodies may reference only: port names, parameter names,
`DtPlant` (block-local substep dt), plain math functions
(`cos/sin/sqrt/fabs/fmin/fmax/fmod/round/floor/trunc`), and `M_PI`/`M_SQRT3`.

**Do not** run `Timing::Validator` against plant graphs: it enforces the
firmware global-cycle rules and rejects all plant graphs by design. Plant
graphs have their own scheduler (below).

## Execution semantics, for dummies

The plant graph advances in **integration substeps** (dt handed down by the
host sim, typically 25–200 µs; one control period is 400 µs on the reference
setups). Each substep executes exactly one evaluation pass over the graph.
Nodes are one of three kinds:

- **Host nodes** — the simulator pokes their outputs out-of-band at the start
  of every pass: `Plant.Io.DutyIn` (du/dv/dw from the controller),
  `Plant.Shaft.Dyno` (rotor speed in dyno rigs), `Plant.Fault.Point` (named
  fault flags via `set_fault`).
- **Combinational nodes** — pure functions of their inputs in the current
  substep (Knee3ph, sensors, Loss, the Util nodes...).
- **Stateful nodes** — own integrators and their outputs only move when they
  integrate (PmsmDq, InductDq, DcBus, FreeInertia, TwoNodeCauer).

The scheduler is a *freshness* loop:

1. A **combinational** node becomes ready once *every stateful node upstream
   of it* has already integrated this substep, and its combinational ancestors
   have evaluated.
2. A **stateful** node becomes ready once its combinational ancestors have
   evaluated and its stateful ancestors have integrated.
3. Ready nodes evaluate immediately; **declaration order breaks ties**.
4. A consumer reads the producer's output value *as of the moment it
   evaluates*: **POST** (post-integration, this substep) if the producer
   already integrated, **PRE** (output latched from the previous substep,
   i.e. stored state) otherwise.

### Cycle-lock, worked through the Knee/Machine/DcBus loop

The bench graph (`gen7_bench_pmsm.json`) wires a real algebraic loop:

```
Knee.V_U/V_V/V_W  -->  Machine.V_U/V_V/V_W        (voltages drive integration)
Machine.I_U/I_V/I_W --> Knee.I_U/I_V/I_W          (currents shape the IGBT knee)
DcBus.Vdc --> Knee.Vdc
Machine.P_Elec + Loss.P_Loss --> Sum(Draw) --> DcBus.P_Draw
```

Walk one substep:

1. `DutyIn`, `Dyno`, `Fault.Point` emit (host-assigned).
2. Stall. `Knee` (combinational) needs the Machine's phase currents, but the
   Machine is stateful and hasn't integrated yet; the Machine in turn can't
   integrate until `Knee` produces its winding voltages. Nothing is ready —
   the loop has **cycle-locked**.
3. The force rule kicks in: among the pending **stateful** nodes, force the
   one with the **fewest pending stateful ancestors**. The Machine has zero
   stateful ancestors; the DcBus has one (the Machine, through
   `P_Elec → Draw → P_Draw`). So the **Machine is forced**: its combinational
   ancestors (`Knee`) evaluate against **PRE** values — the Machine's currents
   and the bus voltage *as latched from the previous substep* — then the
   Machine integrates. Its outputs are now POST.
4. Freshness takes over again: `Loss`, `Hypot`, `AbsOmega`, `Draw` evaluate
   (now against POST machine values), `DcBus` integrates, `TwoNodeCauer`
   integrates, sensors evaluate, `MeasOut` collects.

This reproduces the monolithic twin's imperative substep order
(`twin/plant/plant.py`) *exactly*, which is why the two implementations can be
compared bit-for-bit (see §Validation). Because the schedule is
order-deterministic per graph, the emitter freezes it at codegen time.

### Authoring rules the contract depends on

- Stateful integrators are plain Euler (`X += dX_dt * dt;`) but you must
  compute **all derivatives from the pre-update state into temporaries first,
  then assign** — assigning `id` before computing `diq` silently breaks
  bit-comparability with the reference.
- Any node whose state can escape numerically must clamp inside its own body
  (the python reference `twin/plant` is the rail catalogue).
- Unconnected optional inputs zero-bind. `Plant.Fault.Point` outputs default
  to 0 and are raised by the host by name (`set_fault`).

### Generated surface

From a plant graph the emitter (Lib/PlantCodegen, in-flight) generates:

- `struct PlantState` — one field per persistent output/state per node,
- `void PlantInit(PlantState&)` — constructor bodies + parameter defaults,
- `void PlantStep(PlantState&, double dt)` — all bodies in the frozen
  schedule above.

The hardware seam is the pair of `isEntryPoint: true` pass-throughs:
`Plant.Io.DutyIn` (controller duties in) and `Plant.Io.MeasOut` (phase
currents / vdc / encoder counts / temps / dyno speed out, collected into a
`PlantMeas` struct the host reads per step).

## The v1 template library (17 nodes)

Namespace `Plant.<Group>.<Name>`; groups: `Io`, `Inverter`, `Machine`,
`Shaft`, `DcBus`, `Loss`, `Thermal`, `Sensor`, `Fault`, `Util`. Authoritative
parameter lists (with per-parameter descriptions, units in the text) live in
each folder's `node.json`.

| Node | Semantics |
|---|---|
| `Plant.Io.DutyIn` | du/dv/dw external duty inputs (host-assigned) |
| `Plant.Io.MeasOut` | measurement collector (PlantMeas seam) |
| `Plant.Inverter.Knee3ph` | duty+vdc -> phase volts with IGBT knee + deadtime + Rce |
| `Plant.Machine.PmsmDq` | PMSM dq-Euler, torque, angle, abc currents, open-gate collapse |
| `Plant.Machine.InductDq` | induction machine, stationary-frame coupled-L (V/Hz) |
| `Plant.Shaft.Dyno` | external rotor-speed input (host-assigned) |
| `Plant.Shaft.FreeInertia` | J/B shaft integration from machine torque |
| `Plant.DcBus` | link capacitor + bench supply (CV / source / sink / CV-clamp) |
| `Plant.Loss.FittedLaw` | v2 total-loss law + zero floor + plate share + idle heat |
| `Plant.Thermal.TwoNodeCauer` | plate -> heatsink -> ambient + NTC spot offsets |
| `Plant.Sensor.Hall` | quantize + clamp + isolated-rail phantom mode |
| `Plant.Sensor.VoltageSense` | first-order lag + 0.44 V LSB quantization |
| `Plant.Sensor.SinCosEncoder` | theta -> bounded counts, disconnect rails |
| `Plant.Fault.Point` | host-raised named fault flags (OpenGates, EncoderDisconnect, SensorRail) |
| `Plant.Util.Sum` | Out = A+B+C (C optional) |
| `Plant.Util.Hypot` | Out = hypot(A, B) |
| `Plant.Util.AbsOmega` | Om = abs(Omega_Elec)/Poles |

Physics sources of truth: `twin/plant/plant.py`, `twin/plant/induction.py`
(python reference), and the recorded campaign data
(`Documentation/Docs/Safety-and-Compliance/Testing/`).

## The three example plants

All in `Assets/PlantExamples/`; none use bridges (plant graphs never do).

**`gen7_bench_pmsm.json`** — 17 nodes / 44 connections. The Gen7 "Size 2"
bench article: ZF75-10 PMSM held by the dyno, CM600DY-class knee inverter,
19.8 mF DC link with bench CV supply, fitted v2 loss law, two-node
plate→heatsink→ambient thermal assembly (defaults = the pasted-interface net),
hall + MAX22530 vdc-sense + sin/cos encoder views, and a fault point for
gate-kill and guardrails. The bench-supply model carries the real bench
classes (source/sink limits, bus rise on regen, collapse under slip demand).
Mirrors the calibration targets proven against the steady-state and fault
campaigns.

**`teco_vhz_induction.json`** — 16 nodes / 38 connections. The TECO MAX-IE3
induction bench: V/Hz-driven IM (`InductDq`) with the same knee inverter and
DC-link/bench-supply model (source/sink limits cover the 180 V bringup
collapse episode), a free-spinning shaft (`FreeInertia` — this rig has no
encoder), hall + MAX22530 sensor views, fault point. Mirrors the bringup /
no-load / reversal campaigns.

**`zero_dsr_zf75_7.json`** — 17 nodes / 44 connections, same topology as the
bench graph. Placeholder for the 2018 Zero DSR road pack (Zero ZF75-7 PMSM;
the dyno node serves as drive-cycle replay). Rail-voltage class ~100 V with
road peaks including the 900 A runs. Parameters are marked TBD until the
riding telemetry lands in `twin/recordings/`; once it does, replace the
parameter pack and thermal assembly with the bike's measured values.

## Validation story

### What is proven

The python reference evaluator `twin/plantgraph/plantgraph.py`
(`PlantGraphSim`, `_run_pass`) implements the contract above and is
**format-proven against the certified monolithic plant** — the same plant that
reproduces all 22 recorded fault-campaign scenarios (twin ROADMAP M5). Two
proofs, both currently **PASS**:

```bash
# run from the workspace root (not from twin/):
PYTHONPATH=. python3 -m twin.plantgraph.proof_bench_parity
PYTHONPATH=. python3 -m twin.plantgraph.proof_teco_parity
```

Reports land in `twin/out/plantgraph_proof.json` and
`twin/out/plantgraph_proof_teco.json`.

**Bench parity** (`proof_bench_parity`, graph `gen7_bench_pmsm.json` vs
`twin.plant.plant.Plant`): an identical scripted campaign is driven through
both — dq voltage commands converted to phase duties around the rotating
rotor frame (quasi-controller waveforms; a fixed-duty DC vector would rail
the current integrators on a spinning machine), speed steps 500→2000 rpm, an
OpenGates kill mid-segment with re-latch, and a thermal soak. 21000 control
periods, 20520 compared (the 480 periods after commanded changes are excluded
for electrical settle, L/R tau ~8.7 ms). Peak commanded current 1017.7 A.
Highlights at ≤1500 rpm (strict band):

- **Encoder sin/cos counts: max difference 0** — bit-exact (tolerance 60
  counts).
- dq/phase currents: max |Δ| 3.5–4.8 A against tolerances of 8/8.5 A + 2%.
- vdc: max |Δ| 0.44 V (tolerance 0.75 V). Inverter plate temp: max |Δ|
  0.0138 °C. DC-link power: max |Δ| 217.6 W within 3 % relative tolerance.
- The OpenGates collapse (a pure time-constant decay) is asserted inside the
  strict band — zero divergence between implementations.

Overall verdict: **PASS**.

**TECO IM parity** (`proof_teco_parity`, graph `teco_vhz_induction.json` vs
`twin.plant.induction.InductionPlant`): identical V/Hz patterned duty stream
(rotating vector at 30 Hz, fixed 18 V magnitude), free shaft, starved 1 A
bench supply — the bringup collapse regime — at 50 µs substeps. The IM's slow
rotor modes keep the pair out of the PMSM's dephasing-sensitive corner, so
tolerances are tight:

- isd/isq: median diff ≈ **0.9 mA**, max 11.8–13.5 mA (tolerance 0.5 A).
- vdc: max 0.6 mV. Shaft rpm: max 0.0029 rpm. Measured phase currents:
  median diff exactly 0, max 0.07 A.
- `dsr_smoke` (the Zero placeholder graph instantiates and steps with healthy
  encoder bounds): **PASS**. Overall verdict: **PASS**.

### What the hi-speed informational band documents

Above 1500 rpm mech (>~100 Hz electrical) the bench proof switches currents
and DC-link power to median-drift statistics (hi band: median |Δ| 4.3–7.1 A,
1.2–3.3 % relative, tolerated up to 40–45 A / 10 %). This is **not a graph
defect**: both plants hold the duty vector frozen for the 400 µs control
period while the rotor advances — the real 2.5 kHz ISR behaves the same way
and the twin inherits it from its averaged model. The forced dq voltage ripple
(~0.5 % of vector magnitude per 6° of per-period advance) lands on a
7.65 mΩ winding, i.e. tens of amps of *forced* response, and two open-loop
implementations dephase-sensitive to it diverge. In the real system the
closed-loop controller absorbs exactly this; the band exists as a wide
regression alarm, and the strict assertions live at low speed where the pair
matches to quantization.

### What remains for closed-loop validation

The format proofs validate the *graph semantics* under scripted open-loop
drive. The remaining step is closed-loop: actual emitted controller firmware
ticking against the graph plant, with session output diffed against recorded
telemetry. The seed is proven — `twin/native/` already runs the real
InverterCodegen-emitted firmware against an equivalent C++ plant through a
full `platform_api` shim and writes schema-parity v3 JSONL (M3a smoke: IqVar
100 @ −120 rpm dyno). In flight on the RTE side: Lib/PlantCodegen (graph →
`PlantStep` emitter) and `rte sim`, which composes both into

```bash
rte sim --sw-graph <controller.json> --plant-graph <plant.json> \
        --base-source Images/Gen6FW --duration S --dyno-rpm R \
        --event 't=.. kind=.. arg=..' --out session.jsonl
```

(python reference for the eventual acceptance diff: the twin's existing
scenario experiments in `twin/experiments/`, 22/22 vs recordings.)

## Authoring a new plant pack

1. **Copy a template folder**, keep `node.json` ports/parameters and the
   `inline.cpp` semantics structure identical to the reference physics — card
   runs must stay green regardless of which engine (python or C++) runs the
   graph. Same for a whole graph: instantiate, wire, set the parameter strings
   (floats-as-strings, exactly like controller graphs).
2. **Fill parameters from recorded campaigns, not wishes.** Every fitted
   number carries provenance in `twin/calib/cal_log.md` (method + anchors —
   e.g. flux linkage 0.02323 Wb from the 200 A motoring/regen energy split,
   the 5-term total-loss law fitted ≤38 W residual over seven working
   points). The underlying evidence is the 25 executed tests under
   `Documentation/Docs/Safety-and-Compliance/Testing/`. The param pack of
   record is `twin/params/gen7_size2.json`; anything marked
   `"confidence": "gap"` is a calibration target, not truth.
3. **Prove it**: add or extend a parity proof next to
   `twin/plantgraph/proof_bench_parity.py`/`proof_teco_parity.py` that drives
   the monolithic plant and your graph with one identical scripted input
   stream, and asserts inside speed-appropriate tolerances. Then run the
   closed-loop `rte sim` diff against the recorded session when it exists.
