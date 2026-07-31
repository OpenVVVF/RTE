# MPC Three-Phase PMSM — Architecture Report

## Executive finding

The upstream RTE repository is a **graph-to-firmware toolchain** for STM32 motor drives. It does **not** currently ship:

- a differential-equation PMSM plant model,
- an eight-state two-level inverter switching model,
- or a closed-loop simulation engine (ngspice plant is roadmap-only).

RTE **does** provide:

- node-graph control composition (`Assets/NodeTemplates/`),
- hardware PWM via duty cycles (`hw.pwm.set_duty`, `math.svpwm`),
- FOC transforms and PI current control,
- motor parameter calibration on hardware,
- timing domains (`adc_isr`, `tim_isr`, `app_loop`, `vsense`).

Because the requested plant/inverter models are absent from RTE, this project adds **`Lib/Simulation/`** in the working copy only. These models follow RTE transform conventions and standard dq PMSM / two-level VSI equations. They are used for verification, MPC development, and closed-loop simulation. **No files in the read-only RTE source tree were modified.**

---

## 1. How an RTE node is defined

| Artifact | Role |
|----------|------|
| `Assets/NodeTemplates/<id>/node.json` | Metadata, ports, parameters, optional forced `domain` |
| `inline.cpp` | Per-step body executed in the node's timing domain |
| `constructor.cpp` | Optional initialization |
| `class_header.h` / `class_definition.cpp` | Optional class-based nodes |

Loaded by `Lib/NodeAPI/src/NodeTemplates.cpp` into `NodeAPI::NodeType`.

## 2. Node inputs and outputs

Declared in `node.json` under `inputPorts` / `outputPorts` with `WireType` (`quantity`, `frame`, `dtype`). Example: `control.pi_current/node.json`.

## 3. Registration / instantiation

- Types: `NodeAPI::LoadNodeTypesFromDirectory()`
- Instances: `graph.AddNode({id, type, domain, parameters})`
- Codegen: `Lib/InverterCodegen/src/CodeGenerator.cpp` emits per-domain `Step()` functions

## 4. Signal exchange

- **Same domain:** `Connection` wires (`state.<node>.<port>`)
- **Cross domain:** `Bridge` with critical-section `store/load`

## 5. Scheduler / execution

Domains are **hardware ISRs / main loop**, not a generic simulator scheduler:

| Domain | Dispatch |
|--------|----------|
| `adc_isr` | Phase current ADC completion |
| `tim_isr` | TIM1 PWM period (control ISR) |
| `app_loop` | Main loop (~100 Hz) |
| `vsense` | Phase voltage reads |

Validated at design time by `Lib/NodeAPI/src/Timing.cpp`.

## 6. Sampling time

No graph-level sample time. Controllers use explicit `Dt` parameters (e.g. `foc_demo.json`: `Dt = 0.0002` s → 5 kHz). PWM default switching: 2.5 kHz period / 5 kHz transistor switching (`pwm.h`).

## 7. PMSM in RTE

**No plant model.** Hardware motor is the plant. Parameters identified via `cal Motor.PMSM.*`. Legacy FOC in `FocControlManager.cpp` / `FocController.cpp`.

**This project:** `Lib/Simulation/include/simulation/PmsmPlant.h` — new dq PMSM plant for simulation.

## 8. Inverter in RTE

**No eight-state switching model.** Graph path:

`V_D/V_Q` → `math.inverse_park` → `math.svpwm` → duty % → `hw.pwm.set_duty` → `platform_pwm_set()`.

**This project:** `Lib/Simulation/include/simulation/TwoLevelInverter.h` — discrete switching states for FCS-MPCC.

## 9. Inverter interface

| Interface | RTE support |
|-----------|-------------|
| Duty cycles (0–100%) | Yes — primary |
| αβ voltage references | Yes — via `math.svpwm` |
| dq voltage references | Yes — via inverse Park |
| Gate / switching states | **No** — requires adapter (new `Control.Mpcc` node outputs Sa/Sb/Sc) |

## 10. Rotor position / speed

- `hw.encoder_angle`: mechanical θ [rad]
- `math.encoder_elec_angle`: θ_e = offset + sign·θ_mech·poles/2
- `hw.encoder.decode`: θ and Ω

## 11. Transforms (RTE graph convention)

```text
I_α = I_a
I_β = (I_b - I_c) / √3
I_d = I_α cosθ + I_β sinθ
I_q = -I_α sinθ + I_β cosθ
```

Matches `Assets/NodeTemplates/math.clarke`, `math.park`, `math.inverse_park`.

## 12. Parameters

`config.value` nodes → FRAM KV store. Motor: `Motor.PMSM.*` from calibration.

## 13. Logging

- On-device: `app.telemetry_log` → TLM1 protocol
- Host: NodeGUI `RuntimeController` (live serial or `--simulate` synthetic sines)

## 14. Examples and tests

| Path | Content |
|------|---------|
| `Assets/Examples/foc_demo.json` | Full FOC graph |
| `Lib/NodeAPI/tests/` | Graph/timing tests |
| `Lib/Simulation/tests/` | **New** inverter/PMSM/MPCC verification |

---

## Simulation architecture added in this project

```text
MpccController → switching state (Sa,Sb,Sc)
      ↓
TwoLevelInverter → v_α, v_β
      ↓
PmsmPlant (dq integration)
      ↓
feedback: i_d, i_q, θ_e, ω_e
```

Closed-loop runner: `Lib/Simulation/simulation/mpcc_closed_loop.cpp`

New RTE node (firmware-oriented): `Assets/NodeTemplates/Control.Mpcc/`
