# HostSim — Implementation Plan

Host-side base image for `RTECodeEmitter`: generated domain code calls
`platform_api.h`; scheduling, sensors, and the **plant** live in this tree.

This document tracks planned work. **Current plant:** discrete-time PMSM ODE in
`src/motor_model.cpp` (averaged duty → dq dynamics → ABC currents). **Optional
PWM scope:** triangle-carrier model in `src/pwm_scope.cpp` (visualization only).

Aligns with the repo roadmap item: [ngspice-based plant/inverter
simulator](https://ngspice.sourceforge.io/) for closed-loop graph testing before
hardware (see root `README.md`).

---

## 1. Goals

| Goal | Notes |
|------|--------|
| Same control graphs as firmware | No changes to node templates / codegen contract |
| Fast interactive path | ODE plant remains default for live NodeGUI + SPWM/FOC demos |
| Optional higher-fidelity electrical plant | ngspice backend behind the same `platform_api` |
| Scenario-driven | Backend and netlist chosen in JSON, not compile-time only |

---

## 2. Architecture (target)

```
scenario JSON
  simulation.plant.backend: "ode" | "ngspice"
        │
        ▼
  SimRuntime::StepOnce()  (10 kHz tim_isr, etc.)
        │
        ▼
  platform_api.cpp  (unchanged signatures)
        │
   ┌────┴─────┐
   ▼          ▼
 OdePlant    NgspicePlant
 (MotorModel) (sharedspice + netlist)
        │
        ▼
  phase currents, encoder angle, vdc → graph / telemetry
```

**Principle:** one scheduler; swap plant backend only. PWM scope may stay a
parallel visualization layer (not required for ngspice v1).

---

## 3. Plant backend interface (Phase 0–1)

Introduce a small C++ seam (names TBD), e.g. `IPlant`:

- `Reset()`, `SetParams(...)` from scenario `motor` / `plant` blocks
- `Step(duty_u, duty_v, duty_w, dt_s)` — called from existing `SimRuntime` path
- Readouts: `id/iq` or `ia/ib/ic`, `theta_e`, `omega_e` (whatever ODE exposes today)

Refactor `MotorModel` behind `OdePlant` without behavior change. `platform_api.cpp`
continues to delegate to the active plant instance.

**Files (expected):**

- `src/plant/plant_backend.h` — interface
- `src/plant/ode_plant.cpp` — wraps `MotorModel`
- `src/sim_runtime.cpp` — construct backend from scenario
- `scenarios/default_motor.json` — explicit `"plant": { "backend": "ode" }` (optional; default ode)

---

## 4. ngspice integration (Phase 2–4)

### 4.1 Integration style

| Approach | Use |
|----------|-----|
| **sharedspice (libngspice)** | In-process stepping; required for closed-loop HostSim + NodeGUI |
| **ngspice CLI** | Optional offline batch / CI smoke; not primary |

CMake: optional `HOSTSIM_ENABLE_NGSPICE` (or detect shared library). HostSim builds
and runs without ngspice when the flag is off or the library is missing.

### 4.2 First netlist scope (v1)

Start small; avoid full Gen6 PCB in v1.

| Option | Models | Fidelity | Speed |
|--------|--------|----------|-------|
| **A — RL load** | 3-phase controlled sources or simplified bridge + RL | Current waveforms vs duty | Best for first hello-world |
| **B — Hybrid** | Spice for inverter voltages; PMSM mechanics still ODE | Medium | Medium |
| **C — Switched PMSM** | MOSFET/IGBT + machine | Highest | Likely too slow for 10 kHz live |

**Plan default:** **A**, then **B** if A proves stable.

### 4.3 Control loop sync

Each `tim_isr` step (default `dt = 100 µs`):

1. Map duty commands → spice sources (voltage or behavioral bridge).
2. Advance ngspice by `dt` (or `N` substeps if netlist requires smaller steps).
3. Sample probe nodes → `ia/ib/ic`, optional `vdc`.
4. Mechanical state: either fixed speed / external ODE, or coupled later.

**Live mode:** expect **slower than real time** for switched models; document that
ODE is the default for interactive `--live`. ngspice v1 can target **offline**
`trace.csv` runs first, then live when performance allows.

### 4.4 Scenario sketch (ngspice)

```json
"plant": {
  "backend": "ngspice",
  "netlist": "plants/inverter_rl.cir",
  "sources": {
    "duty_u": "Vu",
    "duty_v": "Vv",
    "duty_w": "Vw"
  },
  "probes": {
    "i_a": "V(iu)",
    "i_b": "V(iv)",
    "i_c": "V(iw)",
    "vdc": "V(vdc)"
  },
  "substeps": 1
}
```

Paths relative to `Images/HostSim/` or scenario file directory (TBD in Phase 2).

### 4.5 Encoder / sensors

v1: reuse ODE mechanical integrator for `theta_e` / `omega_e`, or hold angle fixed
for RL-load-only tests. Full electro-mechanical coupling is **post-v1**.

ADC/encoder stubs in `platform_api.cpp` continue to read from plant + `SimContext`.

---

## 5. Phased delivery

| Phase | Deliverable | Success criteria |
|-------|-------------|------------------|
| **0** | This plan + `PlantBackend` interface design | Reviewed; no runtime change |
| **1** | ODE behind `IPlant`; scenario `backend: ode` | Bit-identical or equivalent traces vs today |
| **2** | sharedspice loader + minimal RL netlist | Single offline step; currents respond to duty |
| **3** | `SimRuntime` + scenario `backend: ngspice` | Closed-loop emit-and-run on RL plant |
| **4** | Telemetry keys for spice probes | NodeGUI plots spice currents |
| **5** (later) | Hybrid or switched netlist; Gen6-oriented templates | Documented perf limits |

---

## 6. Out of scope (v1)

- Replacing ODE as default plant
- Full PCB parasitics / deadtime-accurate Gen6 netlist
- Mandatory ngspice in CI for HostSim ODE/SPWM tests
- Changes to `platform_api.h` public signatures
- ngspice inside firmware (host-only)

---

## 7. Open decisions (TBD before Phase 2 coding)

Record choices here when made:

| # | Question | Options | Decision |
|---|----------|---------|----------|
| 1 | First netlist | A RL / B hybrid / C switched | _TBD_ |
| 2 | First usage mode | Offline trace only / live NodeGUI | _TBD_ |
| 3 | Platform priority | Windows DLL / Linux-WSL first | _TBD_ |
| 4 | First PR scope | Phase 0–1 only vs through Phase 2 | _TBD_ |

---

## 8. Related docs

- `Images/HostSim/README.md` — run modes, SPWM demo, live TCP
- Root `README.md` — toolchain roadmap
- `Lib/InverterProtocol/` — live telemetry to NodeGUI (orthogonal to plant choice)

---

## 9. Current status (as of plan write)

| Item | Status |
|------|--------|
| ODE PMSM plant | **Done** (`motor_model.cpp`) |
| PWM scope (optional) | **Done** (`pwm_scope.cpp`, scenario opt-in) |
| Live IVP telemetry | **Done** (PR branch) |
| `IPlant` / ngspice backend | **Not started** |
