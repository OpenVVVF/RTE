# Plant Templates — node library for OPEN-VVVF *plant* nodegraphs

This directory holds the **plant-side** node library used by plant nodegraphs
(HW side of the loop), sibling to `Assets/NodeTemplates` (SW/controller side).
The point of the split: the controller graph and the plant graph share the
same node format, but their target is different - the controller graph is code-
generated into firmware; the plant graph is generated into a simulator step
function and is run by `rte sim` (headless) or RTE Studio (UI) in closed loop
with the controller build.

Contract version v1.1 (the state after the four-reviewer dossier round of
2026-10-20: twin commit 5d84e93, RTE commit 5b7ee94 + the sibling
PlantCodegen/RTESim landings of the same round). Decision provenance is cited
as (F-nn / R#) review finding ids.

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
  no bridges - bridges are **always rejected**, by both engines, at load).
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
     stateful node with the fewest pending stateful ancestors is forced
     (excluding self-references; **declaration order breaks ties**): its
     combinational ancestors evaluate against **PRE** values (producer
     outputs from the previous substep, i.e. stored state), then the node
     integrates. Repeat until everything evaluated.
  4. Reading rule: a consumer reads the producer's output value *as of the
     moment it evaluates* — POST (post-integration) if the producer already
     integrated this substep, PRE otherwise. This reproduces the monolithic
     twin's imperative substep order exactly for every graph built from this
     library.
- **t=0 publish (D11).** At init the engines run the schedule once as a
  zero-dt publish pass (python `_initial_eval`; the C++ `PlantInit` mirrors it
  after the constructor bodies): every combinational output and every stateful
  derived view is computed and latched before the first step. The measurement
  view (`sim.meas()` / `PlantCollectMeas`) is therefore **defined at t=0** —
  it is the graph's initialized state, not all-zeros-until-the-first-step.
- **Unwired inputs.** An unwired **required** input is a hard **error** in
  both engines — at graph load in the python reference, at generate time in
  the C++ emitter (loud beats silently-wrong, F-05/R1). Unwired **optional**
  inputs zero-bind — **except `Plant.Io.MeasOut` optional channels**, which
  collect quiet NaN = "unpopulated" (-> JSON `null` in sessions), matching
  the twin harness's `None` channels (D12, F-04). The encoder channels
  `Enc_Sin`/`Enc_Cos` are **optional too**, so encoder-less rigs load
  normally and simply report `null` on those channels (v1.1).
- Stateful integrators are written as plain Euler updates in the node body
  (`X += dX_dt * dt;`) — compute ALL derivatives from the pre-update state
  into temporaries first, then assign (the reference parity found this the
  hard way: assigning `id` before computing `diq` breaks bit-comparability).
  **Gauss-Seidel exception (F-27/F-36):** where the python reference itself is
  sequentially coupled within one substep, mirror its exact statement order —
  `Plant.Thermal.TwoNodeCauer` integrates T1 first and computes T2's
  derivative from the *updated* T1 on purpose; all three engines agree
  *because* they all do it. PHYSICAL RAILS: any node whose state can escape
  numerically must clamp in its own body (reference: twin/plant; every rail in
  the python reference exists with identical literals in the C++ bodies).
- **Numeric regime.** Wires and persistent state are f32 (the node.json
  `dtype: f32` contract; generated `PlantState` fields are `float`), parameter
  literals are exact `double` (verbatim from the graph strings), and bodies
  compute in double, narrowing to f32 on assignment (F-34). Quantization ties
  are **nearest-even** (`rint`, which equals python `round()`), and
  truncation (`int` cast) is used exactly where the reference truncates
  (SinCosEncoder counts). Expect bounded divergence vs the f64 python
  reference evaluator; the golden tapes and format proofs carry the numeric
  budget explicitly (R3 numeric row).

## Host interface (the externals surface)

Nodes participate in three kinds: **host** (assigned out-of-band per substep),
**stateful** (own integrators), **combinational**. The emitter binds externals
by **template id** — `Plant.Io.DutyIn`, `Plant.Io.MeasOut`, plus the scenario
hosts `Plant.Shaft.Dyno` and `Plant.Fault.Point`; a node kind today lives in
the engines' classify tables mirrored against this library, and the C++
emitter hard-rejects a loaded type that the tables don't classify (D3)
(kind-in-schema is a v2 candidate, see twin ROADMAP M6). Any new template
added to this library MUST be registered in the classify table in
`Lib/PlantCodegen/src/PlantGenerator.cpp` (host/stateful/combinational;
machines are stateful) AND mirrored in `twin/plantgraph/plantgraph.py`'s
NODE_CLASSES with the same equations.

**Machine-family conventions.** dq variants (PmsmDq/IPM, SpmsmDq/SPM,
SynRelPmsm/PMa-SyRM) carry the `Poles` pole-PAIR convention throughout;
the physical pole count appears only in the `[CFG]` boot print. CROSS-EDITOR
TRAP (drives dossier 2026-10-21): the CONTROLLER side's `Motor.Poles`
(FRAM/graph config) is the pole COUNT, i.e. 2x this plant pole-PAIR value -
the same machine reads 5 on its plant node and 10 as chassis FRAM Poles; the
foc_demo `CfgPoles` node display carries that callout in its label. SRM
(SwitchedReluctance) has no synchronous frame: `Omega_Elec` on that
machine carries MECHANICAL rad/s, position is per rotor teeth
(`RotorTeeth`), and shaft nodes wired into an SRM graph must use
`Poles=1` to express mechanical speed. Induction (InductDq) is
control-agnostic: the same node serves scalar (V/Hz) and vector/FOC
drive. The `isEntryPoint`
flag is metadata inherited from the controller format; it is informational
here, not the binding key (F-18).

python spelling = `twin/plantgraph/plantgraph.py` setters; C++ spelling = the
generated `plant::` API; `rte sim` spelling = the scenario flags/kinds.

- **DutyIn** — `set_duty(du,dv,dw)` / `PlantSetDuty(state, du,dv,dw)`. The
  closed-loop host assigns the controller's duties once per control period;
  **boot default is 50/50/50 (zero vector)** while unassigned.
- **Dyno** — `set_dyno_rpm(rpm)` / `PlantSetDynoRpm(state, rpm)` /
  `rte sim --dyno-rpm R | --dyno-series 't=rpm,...' | --event 't=..
  kind=dyno-set arg=R'`. The node converts the assigned speed to
  `Omega_Elec = Rpm·2π/60·Poles`, `Rpm`, `Elec_Rpm = Rpm·Poles`; **derived
  views republish on assignment** (at init and inside the setter — equivalent
  to the python per-substep host emit, because host values are constant
  between assignments). The canonical initial-RPM parameter is **`Rpm0`**
  (shared with `FreeInertia`); first-version graphs carrying `StartRpm` are
  tolerated as an alias, with a deprecation warning (D13, F-13).
- **Fault.Point** — `set_fault(name, active)` / `PlantSetFault(state, name,
  active)` / `rte sim --event 't=.. kind=fault-inject|fault-clear
  arg=OpenGates'`. Flags are addressed by **OUTPUT PORT name**
  (`OpenGates`, `EncoderDisconnect`, `SensorRail` — the node's own port
  vocabulary) and **broadcast to all `Fault.Point` instances**; there is no
  per-node selector (the dead string `Name` parameter was removed in v1.1,
  F-26). Python raises on unknown names; the **C++ accessor returns `bool`**
  (false = no instance has a port of that name, D8), so a typo'd scenario is
  detectable (F01/R3). Glossary note: **plant fault names are PHYSICS-LEVEL
  injections** (they bend the plant into the recorded artifact condition);
  the twin's protection-layer vocabulary (`gate_power_cut`, `_rails_dead`,
  `encoder_disconnect`, and the firmware trip names) belongs to the
  harness/firmware side, which decides detection and latch on top of that
  physics (F26 round note).
- **set_input / PlantSetInput** — for **controller-side externals** no plant
  node can produce. `sim.set_input("Omega_Cmd", 188.5)` /
  `PlantSetInput(state, "Omega_Cmd", v)` assigns the value to **every unwired
  input port of that name across the graph** (python `_ext_inputs`; the C++
  setter returns `bool` = at least one match, D7, F-05/F-03) / `rte sim
  --event 't=.. kind=plant-input arg=Omega_Cmd=<rad/s>'` (F13/F-07).
  Canonical user: `InductDq.Omega_Cmd`, the synchronous (command) electrical
  speed whose link side `P_Elec = Torque·Omega_Cmd/Poles` carries the entire
  motoring/regen energy accounting of an IM — the V/Hz controller **must push
  it per control period** (`plant.omega_e_cmd = hz·2π` in the python harness).
- **Supply surface (D9)** — ALL **seven** bench-supply parameters of
  `Plant.DcBus`
  are runtime-mutable: `VSet_V`, `ILimit_A`, `CanSink`, `SinkLimit_A`,
  `CvClamp_V`, `CvClampSink_A`, `SeriesR_mOhm` (F-03/F11: `CvClamp_*`
  included — they drive the Chroma-class regen CV-clamp branch recorded on
  the bench). python: `set_supply(v_set=.., i_limit_a=.., can_sink=..,
  sink_limit_a=.., cv_clamp_v=.., cv_clamp_sink_a=.., series_r_mohm=..)`
  (per-key opt-in; a `None`/absent key is left untouched). C++:
  `PlantSupplyDefaults(state, ...)` hands back the graph's own DcBus defaults
  so partial host overrides fill from the plant instead of baked constants,
  and `PlantSetSupply(state, ...)` repoints the set. `rte sim`: `--supply
  v_set=..,i_limit=..,sink_limit=..,can_sink=0|1,cv_clamp_v=..,
  cv_clamp_sink_a=..,series_r_mohm=..` (unfilled keys take the graph's
  values via the defaults accessor).
- **Boot identity (D10)** — the generated `PlantBootParams` accessor exposes
  the graph's boot identity (winding `Rs`, pole pairs, encoder offset) so the
  host's boot console / `[CFG]` line reports the actual plant's physics
  rather than hardcoded bench constants (F07).
- **Measurement seam** — `Plant.Io.MeasOut` is a pure collector; the
  generated `PlantMeas` carries one `float` field per input port and
  `PlantCollectMeas` fills it (unwired -> quiet NaN -> JSON `null`). The 20
  channels = the 17 platform-facing ones plus the three truth-side power
  diagnostics `P_Heat_W` / `P_Dc_W` / `P_ShaftEm_W` (loss-into-plate, link
  draw, machine air-gap power; they feed the twin harness's `tw_p_heat_w` /
  `tw_p_dc_w` / `tw_p_shaft_em_w` telemetry keys, F04/F09).

## Template authoring ground rules

- Bodies may reference: their **port names**, their **parameter names**,
  internal state they assign, `DtPlant` (block-local substep dt), plain math
  (`cos/sin/sqrt/fabs/fmin/fmax/fmod/rint/floor/trunc`), and `M_PI`/`M_SQRT3`.
  Every referenced identifier must resolve against that set: an undeclared
  read is an **emitter error**, not a consumer-compile surprise (D6b);
  non-finite parameter literals (`inf`, `nan`, `1e999`) are rejected too
  (D4).
- **Locals are declared with `double`/`float`/`int` only (D6a)** — the
  state-field harvest keys on that token set; an unknown local type is an
  emitter error (it must not false-positive into a struct field, R4 traps
  row).
- **`static` locals are forbidden (D6c)** (state that `PlantInit` cannot
  reset; all persistent state must live in `PlantState`, R4).
- Port and parameter names must be valid **C++ identifiers and not C++20
  keywords (D5)** (emitter-validate everything it materializes — ports,
  params, state fields; R4 keywords row); node instance ids likewise.
- **Quantization ties are nearest-even** (`rint`, == python `round()`,
  F-33/F-17); truncation is used only where the reference truncates
  (`SinCosEncoder` counts, deliberately, with a comment).
- Every rail/clamp lives in the node body itself (the python reference is the
  rail catalogue; both engines carry identical literals, V-14).
- **Observability rule (F-31):** a guard that clamps physics out of a fitted
  domain must count its own engagements — `Plant.Loss.FittedLaw.FloorHits` is
  the example (the zero-floor hit counter is persistent state, authored as an
  assigned non-port identifier, and exposed as an output port so
  energy-creation regimes are visible instead of silent).

## Naming conventions

- **Ports:** `Camel_Segments`. Internal 3-phase quantities are schematic
  phase-indexed (`V_U/V_V/V_W`, `I_U/I_V/I_W`); semantic sensors use words
  (`I_True -> I_Meas`, `ThetaE`); speeds split `Omega_Elec` (rad/s elec) from
  `Rpm`/`Elec_Rpm` (F-14: `Omega_Elec` is the machine electrical-speed name
  on both machine nodes — the InductDq port was renamed off `Omega_M`).
- **`Plant.Io.MeasOut` port names mirror the twin's measurement keys
  verbatim** (`Iu_A` -> telemetry `iu_a`, `Temp_Inv1_C` -> `temp_inv1_c`,
  `Enc_Sin` -> encoder counts). They **cannot be renamed without a twin-side
  schema rev** (F-16).
- **Parameters:** take unit suffixes (`_V/_A/_W/_S/_Ohm/_F/_KW/_JK/_C`, also
  `_mOhm`/`_Deg`-style where the unit is not SI-base) **except** the
  documented verbatim-physics exception set, keeping the python physics
  names: `Rs`, `Ld`, `Lq`, `Lambda`, `J`, `B`, `Poles`, `K1..K5`,
  `TdOverhead`, `Off1`, `Off2Idle`, `Off2Slope`, `Off2Knee_C`, `Rpm0`,
  `Id0`/`Iq0`/`Theta0`, `FloorHits` (F-16). `TdOverhead` is the composed
  dimensionless `dt_dead·f_sw` (F-19). Initial RPM is **`Rpm0`** everywhere
  (F-13).
- **Types:** `Plant.<Group>.<Name>` with groups `Io`, `Inverter`, `Machine`,
  `Shaft`, `Loss`, `Thermal`, `Sensor`, `Fault`, `Util` — `Plant.DcBus` is
  the single documented two-segment exception (F-15).

## Parameters / defaults

- `parameters` in the graph node instance are the same floats-as-strings
  used by the controller graphs.
- A parameter referenced by a template body **must be provided by the graph**
  unless it is in the documented default set below; referenced-and-missing
  with no default is a load/emit-time **error** in both engines (R1 param
  row).

| Parameter | Default | Semantics |
|---|---|---|
| `Loss.IdleHeat_W` | 120 | plate heat floor while gates live (cal_log idle-warm +4.9 K anchor; 0 when `Idle >= 0.5`) |
| `Shaft.Rpm0` | 0 | initial mechanical rpm (`Dyno`, `FreeInertia`) |
| `Machine.Id0`/`Iq0`/`Theta0` | 0 | initial dq current / angle (`PmsmDq`) |
| `Sensor.VoltageSense.LagTau_S` | 0 | sensor lag bypass (0 = no lag) |

Defaults today live in the engines' python-parity tables (the python
Node-class `p.get` fallbacks mirrored by the C++ emitter); recording defaults
in the node.json schema itself is a **v2 format candidate** (twin ROADMAP
M6), so node.json remains the authoritative parameter *list*. The engines
also carry transitional implementation fallbacks for the supply block and the
ch2-NTC law — all three shipped graphs state every parameter explicitly, and
new graphs must do the same.

## Graph file keys (plant pipeline)

- **Required:** node `id` + `type` (id = valid C++ identifier; `type` must
  resolve against the loaded library — an unresolvable type is rejected at
  load in both engines).
- **Optional:** `domain` (absent/empty validates as `plant`, F-09);
  `parameters`; `position`/`displayName`/`schemaVersion` are tolerated
  editor/advisory metadata (version enforcement is on the v2 list, F-11).
- **`bridges`: always rejected** (both engines, V-16).
- `excludeFromCompile` / `parameterInputs` / `zeroInputs` are
  **controller-format** keys — an **error** on plant graphs (detection beats
  silent ignore, F-23).

## Current library (v1.1)

| Node | Semantics |
|---|---|
| Plant.Io.DutyIn | du/dv/dw external duty inputs (host-assigned; boot default 50/50/50 zero vector) |
| Plant.Io.MeasOut | measurement collector: 17 platform channels + P_Heat_W/P_Dc_W/P_ShaftEm_W truth diag ports; Enc_Sin/Enc_Cos optional; unwired channels collect NaN -> JSON null (PlantMeas seam) |
| Plant.Inverter.Knee3ph | duty+vdc -> phase volts w/ IGBT knee + deadtime + Rce; optional OpenGates input floats the legs (every voltage view zeroes); V_U_Raw/V_V_Raw/V_W_Raw pre-distortion taps = the phase-ADC view |
| Plant.Machine.PmsmDq | PMSM dq-Euler (IPM), torque, angle, abc currents, open-gate collapse |
| Plant.Machine.SpmsmDq | SPMSM (surface, non-salient): single inductance L_H, torque strictly PM (Ld==Lq by construction) |
| Plant.Machine.SynRelPmsm | PM-assisted synchronous reluctance (PMa-SyRM, e.g. Energica bike class): full PMSM dq-Euler incl. the saliency torque term |
| Plant.Machine.SwitchedReluctance | SRM analytic flux-map machine (3-phase/N-tooth): algebraic psi(i,theta), per-phase winding drive, MECHANICAL rad/s shaft semantics (shaft nodes use Poles=1) |
| Plant.DcDc.BuckBoost2Q | two-quadrant synchronous buck/boost converter (average model + knee-class distortion) |
| Plant.DcDc.DualActiveBridge | Dual-Active-Bridge DC/DC (average square-wave power law with bridge phase shift) |
| Plant.Machine.InductDq | induction machine, stationary-frame coupled-L - serves BOTH scalar (V/Hz) and vector/FOC drive (the 5th-order model is control-agnostic; `teco_vhz_induction.json` exercises the scalar path, an FOC sw graph drives the same node unchanged); Omega_Elec machine speed + Omega_Cmd command-speed external (host set_input) |
| Plant.Shaft.Dyno | external rotor-speed input (host-assigned); Rpm0 canonical initial RPM, derived views republish on assignment |
| Plant.Shaft.FreeInertia | J/B shaft integration from machine torque (Rpm0 initial) |
| Plant.DcBus | link capacitor + bench supply (CV/source/sink/CV-clamp); seven supply params runtime-mutable |
| Plant.Loss.FittedLaw | v2 total-loss law + zero floor + plate share + idle heat; FloorHits guard-engagement counter output |
| Plant.Thermal.TwoNodeCauer | plate -> heatsink -> ambient (Gauss-Seidel substep) + NTC spot offsets; ch2 the load-dependent Off2Idle/Off2Slope/Off2Knee_C law |
| Plant.Sensor.Hall | quantize + clamp; Rail >= 0.5 emits the recorded RailVal_A artifact verbatim (1390/-1250/1180 per channel), bypassing quantization AND the range clamp |
| Plant.Sensor.VoltageSense | first-order lag + LSB quantization (0-lag = bypass); nearest-even ties |
| Plant.Sensor.SinCosEncoder | theta -> bounded counts (int truncation), disconnect rails both channels to 0 |
| Plant.Fault.Point | host-raised named fault flags (OpenGates, EncoderDisconnect, SensorRail), addressed by output port name, broadcast to all instances |
| Plant.Util.Sum | Out = A+B+C (C optional) |
| Plant.Util.Hypot | Out = hypot(A, B) |
| Plant.Util.AbsOmega | Om = abs(Omega_Elec)/Poles |

Authoritative per-node parameter lists are in each folder's `node.json`.
Physics sources of truth are the python reference (`twin/plant/plant.py`,
`twin/plant/induction.py`) and the recorded campaign
(Documentation/.../Testing). New templates: EXACT same structure; keep
semantics identical so card runs stay green regardless of which engine
(python or C++ plant) runs the graph.
