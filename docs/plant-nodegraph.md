# Plant nodegraphs — the HW side of the OPEN-VVVF co-simulation

This is the canonical description of the *plant* nodegraph: what it is, how it
executes, what ships in the v1.1 library, and how it is validated. For the
binding wording of the execution contract itself see
`Assets/PlantTemplates/README.md` (§Execution semantics, §Host interface);
this document explains the why and the worked details around it.

## Why the plant is a nodegraph

The controller you develop in RTE is a nodegraph that gets code-generated into
a firmware image. The plant — inverter, machine, DC bus, sensors, thermals —
is a nodegraph too, with the **same file format but a different target**:

- A **controller graph** (`Assets/NodeTemplates`, domain-timed, with bridges)
  compiles through InverterCodegen into firmware C++.
- A **plant graph** (`Assets/PlantTemplates`, single implicit `plant` domain,
  no bridges — bridges are always rejected at load) compiles into a
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

Template bodies may reference only: port names, parameter names, internal
state they assign, `DtPlant` (block-local substep dt), plain math functions
(`cos/sin/sqrt/fabs/fmin/fmax/fmod/rint/floor/trunc`), and `M_PI`/`M_SQRT3`.

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
  integrate (PmsmDq, InductDq, DcBus, FreeInertia, TwoNodeCauer,
  VoltageSense).

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

At init the whole schedule is also run once as a **zero-dt publish pass**, so
the measurement view is defined from t=0 (see the contract's t=0 publish
clause).

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
   integrate until `Knee` produces its winding voltages. `AbsOmega` is the
   only thing that drains (its cone is host-only). The rest of the loop has
   **cycle-locked**.
3. The force rule kicks in: among the pending **stateful** nodes, force the
   one with the **fewest pending stateful ancestors** (self excluded). At
   this stall in gen7, THREE stateful nodes are tied at exactly one pending
   stateful ancestor: the **Machine** (its cone reaches the DcBus through
   `Knee.Vdc`, plus a self-loop through `Knee.I_*` — self doesn't count), the
   **DcBus** (its cone reaches the Machine through
   `P_Elec → Draw → P_Draw`), and the Vdc-channel **VoltageSense** (its cone
   ends at the DcBus). The count alone cannot decide, so the **declaration
   order tie-break** fires — the Machine is declared earliest in the graph
   and is forced. Its one combinational ancestor (`Knee`) evaluates against
   **PRE** values — the Machine's currents and the bus voltage *as latched
   from the previous substep* — then the Machine integrates. Its outputs are
   now POST. (Re-declaring DcBus ahead of the Machine flips which node is
   forced, in both engines — the tie-break is load-bearing, not cosmetic.)
4. Freshness drains what unblocks (`Im`/Hypot, the hall sensors, the
   encoder — all against POST machine state), then stalls again on the loop:
   the DcBus's cone (`Loss`, `Draw`) still can't see the bus. Second force:
   the DcBus now has **zero** pending stateful ancestors (the Machine is
   done), so `Loss` and `Draw` evaluate against the PRE bus voltage and the
   POST machine values, and the DcBus integrates.
5. Freshness takes over for good: `Knee`'s phase views (`V_*_Raw` and the
   distorted `V_*`) evaluate against the POST bus, `TwoNodeCauer` integrates,
   the voltage-sense chain integrates, `MeasOut` collects.

This reproduces the monolithic twin's imperative substep order
(`twin/plant/plant.py`) *exactly*, which is why the two implementations can be
compared bit-for-bit (see §Validation). Because the schedule is
order-deterministic per graph, the emitter freezes it at codegen time
(the C++ and python schedulers produce the same frozen order; the schedule
of the shipped gen7 graph is `AbsOm Knee Machine Im HallU HallV HallW Enc
Loss Draw DcBus Thermal VSense VSenseU VSenseV VSenseW Meas`).

### Authoring rules the contract depends on

- Stateful integrators are plain Euler (`X += dX_dt * dt;`) but you must
  compute **all derivatives from the pre-update state into temporaries first,
  then assign** — assigning `id` before computing `diq` silently breaks
  bit-comparability with the reference. Exception: where the reference itself
  is sequentially coupled within a substep (`TwoNodeCauer` computes T2's
  derivative from the updated T1), mirror the reference's exact statement
  order — the Gauss-Seidel coupling is the physics of record.
- Any node whose state can escape numerically must clamp inside its own body
  (the python reference `twin/plant` is the rail catalogue).
- An unwired *required* input is a load-time error; unwired *optional*
  inputs zero-bind, except MeasOut channels which collect NaN -> JSON null.
  `Plant.Fault.Point` outputs default to 0 and are raised by the host by
  output-port name (`set_fault`).

### Generated surface

From a plant graph the emitter (Lib/PlantCodegen) generates:

- `struct PlantState` — one field per persistent output/state per node
  (float32 state/wires; parameter literals are exact doubles; the seven
  DcBus bench-supply params are mutable `double` fields),
- `void PlantInit(PlantState&)` — constructor bodies + parameter defaults +
  the zero-dt publish pass (measurement view defined at t=0),
- `void PlantStep(PlantState&, double dt)` — all bodies in the frozen
  schedule above,
- `struct PlantMeas` + `void PlantCollectMeas(const PlantState&, PlantMeas&)`
  — one field per `Plant.Io.MeasOut` input (unwired channels read NaN,
  serialized as JSON `null`),
- the host setters: `PlantSetDuty`, `PlantSetDynoRpm` (republishes the
  derived speed views on assignment), `PlantSetFault` (returns `bool`),
  `PlantSetInput` (assigns every unwired input of the given name; returns
  `bool`), `PlantSetSupply` (all seven bench-supply knobs, CvClamp
  included), plus the `PlantSupplyDefaults` and `PlantBootParams` accessors
  (graph defaults / boot identity for the host).

The hardware seam is the pair of reserved externals: `Plant.Io.DutyIn`
(controller duties in) and `Plant.Io.MeasOut` (phase currents / vdc / phase
volts / encoder counts / temps / dyno speed / dclink power / truth-side power
diagnostics out, collected into the `PlantMeas` the host reads per step).
See the contract's §Host interface for the exact semantics and the
python/C++/CLI spellings.

## The v1.1 template library (17 nodes)

Namespace `Plant.<Group>.<Name>`; groups: `Io`, `Inverter`, `Machine`,
`Shaft`, `Loss`, `Thermal`, `Sensor`, `Fault`, `Util` — with `Plant.DcBus`
as the single documented two-segment exception. Authoritative
parameter lists (with per-parameter descriptions, units in the text) live in
each folder's `node.json`.

| Node | Semantics |
|---|---|
| `Plant.Io.DutyIn` | du/dv/dw external duty inputs (host-assigned; boot default 50/50/50) |
| `Plant.Io.MeasOut` | measurement collector (PlantMeas seam): 17 platform channels + `P_Heat_W`/`P_Dc_W`/`P_ShaftEm_W` truth-side diag ports; encoder channels optional (encoder-less rigs read `null`); unwired channels are NaN, not 0 |
| `Plant.Inverter.Knee3ph` | duty+vdc -> phase volts with IGBT knee + deadtime + Rce; optional `OpenGates` input floats the legs (all voltage views zero); `V_U_Raw/V_V_Raw/V_W_Raw` pre-distortion taps wired to the phase-ADC sense chain |
| `Plant.Machine.PmsmDq` | PMSM dq-Euler, torque, angle, abc currents, open-gate collapse |
| `Plant.Machine.InductDq` | induction machine, stationary-frame coupled-L (V/Hz); `Omega_Elec` machine speed + `Omega_Cmd` controller-pushed command speed |
| `Plant.Shaft.Dyno` | external rotor-speed input (host-assigned; `Rpm0` canonical, legacy `StartRpm` tolerated) |
| `Plant.Shaft.FreeInertia` | J/B shaft integration from machine torque |
| `Plant.DcBus` | link capacitor + bench supply (CV / source / sink / CV-clamp); all seven supply params runtime-mutable |
| `Plant.Loss.FittedLaw` | v2 total-loss law + zero floor (with `FloorHits` engagement counter) + plate share + idle heat |
| `Plant.Thermal.TwoNodeCauer` | plate -> heatsink -> ambient + NTC spot offsets; ch2 the load-dependent `Off2Idle/Off2Slope/Off2Knee_C` law |
| `Plant.Sensor.Hall` | quantize + clamp; `Rail >= 0.5` emits the recorded `RailVal_A` artifact verbatim (1390/-1250/1180 per channel), bypassing quantization and the range clamp |
| `Plant.Sensor.VoltageSense` | first-order lag + 0.44 V LSB quantization (0-lag bypass; nearest-even ties) |
| `Plant.Sensor.SinCosEncoder` | theta -> bounded counts, disconnect rails both channels to 0 |
| `Plant.Fault.Point` | host-raised named fault flags (OpenGates, EncoderDisconnect, SensorRail), addressed by output port name, broadcast to all instances |
| `Plant.Util.Sum` | Out = A+B+C (C optional) |
| `Plant.Util.Hypot` | Out = hypot(A, B) |
| `Plant.Util.AbsOmega` | Om = abs(Omega_Elec)/Poles |

Physics sources of truth: `twin/plant/plant.py`, `twin/plant/induction.py`
(python reference), and the recorded campaign data
(`Documentation/Docs/Safety-and-Compliance/Testing/`).

## The three example plants

All in `Assets/PlantExamples/`; none use bridges (plant graphs never do).
All three wire the phase-ADC sense chain (Knee `V_*_Raw` -> 1.5 ms-lag
`VoltageSense` per phase -> `Meas.Vu_V/Vv_V/Vw_V`) and the truth-side
diagnostics (`Loss.P_Plate -> P_Heat_W`, `Draw.Out -> P_Dc_W`,
`Machine.P_Elec -> P_ShaftEm_W`); unwired channels (`Temp_Inv3_C`,
`Temp_Motor_C`, and the encoder pair on the encoder-less rig) read `null` by
design.

**`gen7_bench_pmsm.json`** — 20 nodes / 54 connections. The Gen7 "Size 2"
bench article: ZF75-10 PMSM held by the dyno, CM600DY-class knee inverter,
19.8 mF DC link with bench CV supply, fitted v2 loss law, two-node
plate→heatsink→ambient thermal assembly (ch2 on the recorded load-relief
NTC law), hall sensors with the per-channel recorded rail artifacts,
MAX22530 vdc-sense + phase-voltage sense chain, sin/cos encoder views, and a
fault point for gate-kill and guardrails. The bench-supply model carries the
real bench classes (source/sink limits, bus rise on regen, collapse under
slip demand). Mirrors the calibration targets proven against the
steady-state and fault campaigns.

**`teco_vhz_induction.json`** — 19 nodes / 48 connections. The TECO MAX-IE3
induction bench: V/Hz-driven IM (`InductDq`; the V/Hz controller pushes
`Omega_Cmd` through the `plant-input` external) with the same knee inverter
and DC-link/bench-supply model (source/sink limits cover the 180 V bringup
collapse episode), a free-spinning shaft (`FreeInertia` — this rig has no
encoder, so `Enc_Sin`/`Enc_Cos` stay `null`), hall + MAX22530 sensor views,
fault point. Mirrors the bringup / no-load / reversal campaigns.

**`zero_dsr_zf75_7.json`** — 20 nodes / 54 connections, same topology as the
bench graph. Placeholder for the 2018 Zero DSR road pack (Zero ZF75-7 PMSM;
the dyno node serves as drive-cycle replay). Rail-voltage class ~100 V with
road peaks including the 900 A runs. Parameters are marked TBD until the
riding telemetry lands in `twin/recordings/`; once it does, replace the
parameter pack and thermal assembly with the bike's measured values.

## Name-mapping glossary (python / C++ / rte sim / twin harness)

The same external reached from four surfaces (see the contract §Host
interface for the semantics):

| External | python (plantgraph) | C++ (generated `plant::`) | `rte sim` | twin harness (python monolith) |
|---|---|---|---|---|
| Phase duties | `sim.set_duty(du,dv,dw)` (boot default 50/50/50) | `PlantSetDuty(state,du,dv,dw)` | (driven per control period by the closed-loop shim) | `plant.duty_u/v/w` |
| Dyno speed | `sim.set_dyno_rpm(rpm)` | `PlantSetDynoRpm(state,rpm)` (derived views republish) | `--dyno-rpm R`, `--dyno-series 't=rpm,...'`, `--event 't=.. kind=dyno-set arg=R'` | `plant.set_dyno_speed(omega_e)` |
| Plant fault flags | `sim.set_fault("OpenGates", True)` (raises on unknown) | `PlantSetFault(state,name,active)` -> `bool` | `--event 't=.. kind=fault-inject arg=OpenGates'` / `kind=fault-clear` | physics-level names map onto harness vocabulary: `gate_power_cut` = OpenGates, `encoder_disconnect` = EncoderDisconnect, `_rails_dead` = SensorRail (+ the protection-layer trip names, which are harness-side, not plant) |
| Bench supply | `sim.set_supply(v_set=..,i_limit_a=..,can_sink=..,sink_limit_a=..,cv_clamp_v=..,cv_clamp_sink_a=..,series_r_mohm=..)` (per-key opt-in) | `PlantSetSupply(state,...)` + `PlantSupplyDefaults(state,...)` (all seven knobs) | `--supply v_set=..,i_limit=..,sink_limit=..,can_sink=..,cv_clamp_v=..,cv_clamp_sink_a=..,series_r_mohm=..` | `plant.supply.update(...)` (same key names as the python graph setter) |
| Controller-side input | `sim.set_input("Omega_Cmd", 188.5)` (all unwired matches) | `PlantSetInput(state,name,value)` -> `bool` (all unwired matches) | `--event 't=.. kind=plant-input arg=Omega_Cmd=<rad/s>'` | `plant.omega_e_cmd = ctrl.y["hz"]*2π` (pushed per control period) |
| Measurement view | `sim.meas()` (dict keyed by MeasOut port names; unwired = NaN) | `PlantCollectMeas(state, meas)` into `struct PlantMeas` | session telemetry channels (`iu_a`, `vdc_v`, `enc_sin`, `tw_p_heat_w`, ...) | `plant.meas` dict (same channel names as the twin telemetry keys) |

## Validation story

### What is proven

The python reference evaluator `twin/plantgraph/plantgraph.py`
(`PlantGraphSim`, `_run_pass`) implements the contract above and is
**format-proven against the certified monolithic plant** — the same plant that
reproduces all 22 recorded fault-campaign scenarios (twin ROADMAP M5). Two
proofs, both currently **PASS** (extended coverage since v1.1):

```bash
# run from the workspace root (not from twin/):
PYTHONPATH=. python3 -m twin.plantgraph.proof_bench_parity
PYTHONPATH=. python3 -m twin.plantgraph.proof_teco_parity
```

Reports land in `twin/out/plantgraph_proof.json` and
`twin/out/plantgraph_proof_teco.json` (regenerated artifacts — the gate is
the PASS verdict, not the bytes).

**Bench parity** (`proof_bench_parity`, graph `gen7_bench_pmsm.json` vs
`twin.plant.plant.Plant`, current committed report): an identical scripted
campaign is driven through both — dq voltage commands converted to phase
duties around the rotating rotor frame (quasi-controller waveforms; a
fixed-duty DC vector would rail the current integrators on a spinning
machine), speed steps 500→2000 rpm, an OpenGates kill mid-segment with
re-latch, a **SensorRail segment** (the recorded isolated-rail artifacts
+1390/-1250/+1180 sail straight through the rail checks exactly), an
**EncoderDisconnect segment** (counts rail to 0 on both), and a thermal
soak. 23750 control periods, 23090 compared (the 660 periods after commanded
changes are excluded for electrical settle, L/R tau ~8.7 ms). Peak commanded
current 1017.7 A. Highlights at ≤1500 rpm (strict band):

- **Encoder sin/cos counts: max difference 0** — bit-exact (tolerance 60
  counts), including across the disconnect window.
- dq currents: max |Δ| 4.79 A (id), 4.42 A (iq) against tolerance 8 A + 2 %;
  phase currents max |Δ| 4.55 A (iu), 3.5 A (iw) against 8.5 A + 2 %.
- vdc: max |Δ| 0.44 V (tolerance 0.75 V). vu_v (the phase-ADC channel added
  in v1.1): max |Δ| 1.32 V against 1.5 V + 5 %.
- Inverter plate temp: max |Δ| 0.0157 °C (tolerance 0.05 °C); **temp2 is now
  asserted too** (load-law ch2): max |Δ| 0.0117 °C against 0.05 °C + 1 %.
- DC-link power: max |Δ| 217.6 W within 3 % relative tolerance.
- The OpenGates collapse (a pure time-constant decay) and the rail/
  disconnect artifact values are asserted inside the strict band — zero
  divergence between implementations.

Overall verdict: **PASS**.

**TECO IM parity** (`proof_teco_parity`, graph `teco_vhz_induction.json` vs
`twin.plant.induction.InductionPlant`): identical V/Hz patterned duty stream
(rotating vector at 30 Hz, fixed 18 V magnitude), free shaft, starved 1 A
bench supply — the bringup collapse regime — at 50 µs substeps (`Omega_Cmd`
pushed through the `set_input` external). The IM's slow rotor modes keep the
pair out of the PMSM's dephasing-sensitive corner, so tolerances are tight:

- isd/isq: median diff ≈ **0.9 mA**, max 13.5 / 11.8 mA (tolerance 0.5 A).
- vdc (truth): max 0.6 mV. **The measured `Vdc_V` sense channel is now
  asserted too** (0.44 V/LSB quantization class): max diff 0.0 (tolerance
  0.45 V). Shaft rpm: max 0.0029 rpm. Measured phase currents: median diff
  exactly 0, max 0.07 A (tolerance 0.15 A).
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

### The C++-engine test landscape

- `PlantCodegen_tests` (Lib/PlantCodegen, 16 gtest cases): scheduler frozen
  orders (asserted equal to the python-derived schedules), the validation
  matrix (identifier/domain/maxInstances/wiring/param errors, bridges,
  combinational cycles, all three example graphs accepted), and the gen7
  golden tape — C++-generated plant vs the python reference evaluator,
  including the supply repoint and the OpenGates collapse window. The three
  example graphs are also emitted and compiled as `plant_golden_*` libraries
  with zero warnings under `-Wall -Wextra -Wpedantic -Werror`.
- `RTESim_tests` (Source/RTESim, 11 gtest cases): scenario/event/supply/
  dyno-series parsing, session-start + record schema byte-compatibility with
  `twin/harness.py`'s TwinSession.
- `RTESim_e2e_smoke` (landing in this round): the `rte sim` closed-loop
  smoke as a ctest — emits the real controller firmware and the real plant,
  runs the standard gen7 smoke card, and loads the session back through
  `twin.replay`.
- `twin/native/tools/parity.py`: the python-twin-vs-native-C++ twin gate
  (M3a), kept as the monolith-side reference for the same smoke scenario.

Full suite before this review round: 123/123 ctest.

### What remains for closed-loop validation

The format proofs validate the *graph semantics* under scripted open-loop
drive, and the loop itself now runs end-to-end (`rte sim`, landed — real
controller firmware + graph-generated plant + schema-parity v3 JSONL). What
is still open:

- **Real-firmware protection verification (F02, in flight):** fault
  injects now raise real plant physics, but the *trip* side (gate_ready /
  control_state / fault_flags / `[FAULT]` vocabulary from the actual emitted
  firmware fault manager, diffed against recorded trip telemetry) is the
  open verification item — the twin's protection layer is harness-side and
  the harness-fault-vs-plant-fault glossary in the contract applies.
- **Event-vocabulary parity:** `var_series` (scripted var ramps), `config
  set` (live FRAM writes), and `reboot` event kinds from the twin harness
  vocabulary are not yet on the `rte sim` timeline.
- **The Studio tab:** `docs/studio-sim-tab.md` (design intent; drives `rte
  sim` through the automation session).

## Authoring a new plant pack

1. **Copy a template folder**, keep `node.json` ports/parameters and the
   `inline.cpp` semantics structure identical to the reference physics — card
   runs must stay green regardless of which engine (python or C++) runs the
   graph. Same for a whole graph: instantiate, wire, set the parameter strings
   (floats-as-strings, exactly like controller graphs). Mind the ground rules
   in the contract (locals `double/float/int` only, no `static`, identifiers
   not C++20 keywords, nearest-even quantization) and whether the new node is
   host/stateful/combinational — node kinds live in the engines' classify
   tables today, so a new template lands together with its registry entries
   (kind-in-schema is on the v2 list, twin ROADMAP M6).
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
   closed-loop `rte sim` diff against the recorded session.
