# Plan: Scalable Multi-Modulator Motor Control Firmware (STM32H723ZG)

**Status:** Investigated against the current Gen6FW base image + RTE toolchain (2026-08).
**Hardware verified:** STM32H723ZGTx (`.ioc`), DS13313 Rev 4 pin/AF tables.

Each section ends with a **Feasibility** verdict grounded in the actual codebase.

---

## 1. Architecture: Four Layers

```
Application   — speed loop, state machine (app_loop domain, ~100 Hz–1 kHz)
Controller    — FOC current loop (tim_isr domain, 10 kHz dual-update)
Modulator     — swappable strategy: SVPWM / SPWM / SHEPWM / RCFM
PWM Driver    — owns all TIM1 registers, shadow commits, dead time, trip
```

Core principle: the controller outputs `Vα, Vβ, θe, fe` and never knows which
modulator is active. Each modulator is a component with four entry points and
private per-instance state:

```c
typedef struct mod mod_t;
typedef struct {
    void (*update)(mod_t *m, float va, float vb, float theta_e, float fe);
    void (*commit)(mod_t *m);                 /* write to TIM1 via pwm driver */
    bool (*enter) (mod_t *m, float theta_e, float mi);  /* phase-locked take-over */
    void (*exit)  (mod_t *m);
} mod_ops_t;
```

**Feasibility: HIGH — and half of it already exists.**

- The PWM driver layer exists (`Src/Inverter/Drivers/PWM/pwm.cpp`): TIM1
  register ownership, dead time, break/MOE handling, `PWM_SetThreePhaseDuty`,
  `PWM_SetVoltageVector` (SVPWM), open-loop SPWM ramp.
- What does *not* exist: the `mod_ops_t` dispatch layer and private instance
  state. Today the "modulator" is an implicit `foc_active`/`spwm_running`
  flag pair in `pwm.cpp`.
- **Constraint:** there are two consumers of the PWM driver — the legacy
  `FocControlManager` (C++ path) and the generated `tim_isr` domain via
  `platform_pwm_set()` (`Actuators.PwmOut` node) / `platform_pwm_set_voltage_vector()`.
  The modulator interface must sit **below** these entry points, not replace
  them, or the codegen contract (`platform_api.h`) breaks.

## 2. Execution Context — mostly already at target

| Plan assumption | Reality in Gen6FW |
|---|---|
| "Current: current sense on free-running timer" | **Wrong.** Already PWM-synchronous: TIM1 CH4 OC4REF → TRGO triggers the ADC1/ADC2 injected dual-simultaneous group at PWM bottom (`PhaseCurrentADC.cpp:118-144`). |
| "Target: ADC-complete ISR does sample → FOC → commit" | Partially present, deliberately split: ADC-complete ISR does sample + safety (`onInjectedConversionComplete`, `PhaseCurrentADC.cpp:326`); FOC runs in the TIM1 update ISR at 10 kHz dual-update (`FocControlManager::onPwmPeriod`, RCR=0). The codegen domains `adc_isr` and `tim_isr` already model this split. Unifying FOC into the ADC ISR is optional, not required. |
| "Target: angle extrapolation θe(t) = θs + ωe·Δt" | **Already done.** `EncoderADC::extrapolatedAngleDeg()` uses DWT-cycle timestamps + RPM EMA (`EncoderADC.cpp:222`), exposed to generated code via `platform_get_encoder_angle_latest()`. Only the legacy `FocControlManager` still reads the raw snapshot (`lastAngle()`) — minor cleanup, not new work. |

**Feasibility: DONE / TRIVIAL.** Section 2 of the original plan is ~90%
already-implemented. Remaining: point the legacy FOC path at the extrapolated
angle and decide whether `commit()` runs from `tim_isr` or `adc_isr` (recommend
keeping the current split).

## 3. Modulator Implementations

**SVPWM / SPWM (async):** exist today in `pwm.cpp` and as graph nodes
(`Transforms.Svpwm` → `Actuators.PwmOut`). `update()` computes duties →
`commit()` writes CCR1..3; ARPE + OC preload are already ON
(`tim.c:51`, HAL PWM channel config), so the three writes land atomically at
the next update event. Wrapping them behind `mod_ops_t` is a pure refactor.

**SHEPWM (sync):** own timebase on **TIM5** (32-bit), not the carrier timer:

- `ARR = f_tim / fe` → one timer cycle = one electrical cycle.
  TIM5 clock = 137.5 MHz (APB1, same as TIM2 per `EncoderADC.cpp:79,91`).
  At 300 Hz: ARR ≈ 458,333 → **7.3 ns edge resolution**, ~8×10⁻⁴ ° angle
  resolution — the plan's numbers hold.
- Angles stored per quarter cycle (quarter-wave symmetry); full cycle by
  reflection. 51-pulse pattern → 51 angles + level list per MI value.
- `angle_to_cnt(α) = α/2π × ARR`.
- Edge fire options, in bring-up order:
  1. **Compare-chain ISR** (write next CCR one edge ahead). At 51 pulses,
     300 Hz: ~30k IRQ/s ≈ 1–2% CPU on a 550 MHz M7. **Start here.**
  2. **DMA circular buffer** — TIM5 CC-event-triggered DMA stream refilling
     TIM5->CCRx from a pattern buffer. Zero CPU. Feasible with the free DMA
     streams (only DMA2_Stream0 is in use for the encoder).
  3. ~~HRTIM~~ — **does not exist on STM32H723** (verified: no HRTIM anywhere
     in DS13313). Note: `docs/MPC_Multi_Rate_Current_Sampling_Plan.md` also
     mentions HRTIM as an option — that is an MCU change, not a redesign.
- TIM1 keeps running center-aligned the whole time: ADC trigger, encoder
  TRGO2 sync, and FOC cadence never change.

**RCFM (random carrier frequency):** nearly free once the interface exists —
vary TIM1 ARR/PSC per period under preload; commit at update event.

**Timer budget (verified):** used = TIM1 (PWM + ADC/encoder triggers), TIM2
(encoder 10 kHz idle trigger; idle during control), TIM3 (100 Hz slow-sensor
scan). **Free: TIM4, TIM5, TIM8 (advanced-control, 16-bit), TIM6/7,
TIM12–17, LPTIMs.** TIM5 is the clean SHE timebase; TIM8 is a spare
advanced timer if a future hardware rev routes pins to it.

**Feasibility: HIGH for SVPWM/SPWM/RCFM; MEDIUM-HIGH for SHEPWM.** The one
real open problem is not timing but **modulation-index continuity**: SHE
angles are solved offline for a *fixed* MI, while FOC demands a continuous
`Vα/Vβ`. Discrete MI steps → torque disturbance. Options (choose during
design, see §9): (a) table family + interpolation, (b) hybrid — SHE edges
plus between-edge SVPWM trim, (c) restrict SHE to the voltage-saturated
high-speed region where the current loop is effectively amplitude-limited
anyway. This is the main control-design risk of the whole project.

## 4. Glitch Safety

**Software:** double-buffer SHE tables (ping-pong); slow task writes `build`,
edge ISR reads `active`; single pointer swap only at electrical-cycle wrap
(TIM5 update event). Never torn mid-cycle.

**Hardware:** already mostly in place — ARPE + CCR preload ON, TIM1 RCR in
use, all mutations at cycle boundary. For SHE forced-mode writes (§6) OCxM
changes take effect immediately — which is exactly what edge placement wants,
but means the *ISR/DMA sequence itself* must be the only writer (guarded by
the pending-flag protocol, §5).

Worst-case failure becomes: "new pattern applies next electrical cycle."

**Feasibility: HIGH.** Matches existing driver invariants; the new part is
only the ping-pong table discipline.

## 5. Supervisor (slow task)

- Selection table: mode = f(fe, mi, quadrant) — data-driven, tunable.
- Hysteresis on every threshold (enter 40 Hz / exit 35 Hz) against chattering.
- Request → `pending` flag → fast context executes at the safe point
  (electrical-cycle wrap for SHE; any update event for async mods).
- SHE entry phase-lock: `enter()` presets TIM5 CNT = `angle_to_cnt(θe)` using
  the **extrapolated** encoder angle, aligns the table cursor, and level-matches
  the initial pin state to avoid a voltage step.

**Feasibility: HIGH.**
- `ControlSupervisor` exists but is a *lifecycle* manager (gate-driver
  sequencing, PWM start/stop, fault response — `ControlSupervisor.h`) — the
  modulation supervisor is a new, separate component that belongs in the
  `app_loop` domain (soft real-time) with the safe-point executor in ISR.
- Tunable transition table: the FRAM KV store + `Values.Config` node +
  `config set/get` shell are the existing mechanism — thresholds can be
  runtime-tunable **today** without new tooling.
- Forced-angle diagnostic (`FocControlManager::setForcedAngleRate`) gives a
  clean synthetic θe ramp — ideal for SHE bring-up before FOC is involved.

## 6. Live Handoff at Speed — Option A is impossible on this hardware

**Verified pin fact:** the six gate pins PE8–PE13 (TIM1 CH1N/1/2N/2/3N/3) have
**TIM1 as their only timer alternate function** (DS13313 Table 7; other AFs
are DFSDM/UART7/SPI4/SAI4/FMC/COMPx_OUT). TIM2/TIM5/TIM8 cannot reach them,
and the H723 has no HRTIM. **GPIO AF re-mux handoff (Option A) is physically
impossible on Gen6.** Drop it.

**Option B — TIM1 always owns the pins (only and safest path):**

- SHE timer (TIM5) never touches GPIO. Its compare events write TIM1's
  output-compare **forced modes**: per edge, write CCMR1 (CH1+CH2 fields) and
  CCMR2 (CH3 field) with `OCxM = FORCE_ACTIVE / FORCE_INACTIVE`.
- MOE stays set the entire time → the **BKIN hardware trip (gate-driver fault
  on PE15) stays armed in every mode** — the original plan's worry about a
  dead break input disappears.
- Dead time: the DTG stage acts on OCxREF transitions including forced-mode
  changes, so complementary dead time stays in circuit. **Must be validated
  on a scope during bring-up** (measure high/low-side overlap at forced
  transitions; boot DT ≈ 705 ns, runtime default 1 µs).
- Edge jitter ≈ ISR latency (~0.5–1 µs with TIM5 CC IRQ at priority above
  `tim_isr`/`adc_isr`, below the AWD/fault IRQs) ≈ 0.11° at 300 Hz — acceptable
  for SHE.
- Exit path: restore `OCxM = PWM1` on all three channels; preloaded CCRs
  resume normal SVPWM at the next update event — no glitch.
- Overcurrent stack is unchanged and mode-independent: gate-driver hardware
  fault → BKIN (ns–µs); ADC1 analog watchdog on injected channels → fault;
  software OC with consecutive-sample voting.

**Feasibility: HIGH — simpler than the original plan**, because the
two-timer ownership dance and its safety caveats are gone. One timer owns the
pins, always; "handoff" is just who writes OCxM/CCR.

## 7. Model-Based / Codegen Mapping

| Architecture piece | Status in RTE today |
|---|---|
| Component blocks w/ per-instance data | **Exists** — class-based node templates (`class_header.h`, `class_definition.cpp`, `constructor.cpp`; see `Assets/NodeTemplates/README.md`, `CodeGenerator.cpp` constructorCode). |
| Async modulators as nodes | **Exists** — `Transforms.Svpwm` + `Actuators.PwmOut`; full FOC chain already runs as a graph (`Assets/Examples/foc_demo.json`: Clarke/Park/PI/InversePark/Svpwm in `tim_isr`, currents in `adc_isr`). |
| Runtime dispatch, one modulation slot | **Missing.** `Logic.Mux` is a *signal* mux — it cannot express component enter/exit (phase-lock, timer arming, safe-point swap). This is the biggest toolchain gap. |
| Supervisor chart | **Missing as a node type.** Expressible today with Var/Gate/Mux + config keys in `app_loop`, or hand-written in the base image. |
| Tunable calibration struct | **Exists** — FRAM KV + `Values.Config` + `config` shell. |
| Atomic regions | **Exists** — `platform_critical_enter/exit` in `platform_api.h`. |
| HAL primitives (forced-mode writes, TIM5 pattern engine, ping-pong tables) | **Missing** — new base-image driver + `platform_api` extensions. Per the plan's own boundary rule this is where the SHE machinery belongs. |
| "Hold-until-safe-point" rate transition | **Missing in codegen**; trivial in base image (pending flag + cycle-wrap check). |
| SIL/HIL harness | **Missing** — ngspice plant sim is roadmap, not built. On-target instruments exist and are permanent: spike recorder, `enc_trace`, `hz_*` rate telemetry, duty readback. |

**Recommended boundary (unchanged from the plan, and confirmed correct by the
codebase):** the modulator *slot* and SHE engine are base-image HAL
primitives; the graph sees (a) the existing async-mod nodes (SVPWM etc.) and
(b) one new config/control surface for the supervisor (mode select as a
signal or config key, thresholds as KV). Do **not** try to express TIM5
compare chains or OCxM forced writes in node code.

**Feasibility: MEDIUM.** Base-image work is straightforward; making the slot
a first-class codegen concept (enter/update/commit/exit lifecycle, safe-point
semantics) is a genuine new toolchain feature and should be deferred until
the base-image implementation is proven on hardware.

## 8. Bring-Up Order (revised)

1. **Modulator interface refactor** — wrap existing SVPWM/SPWM behind
   `mod_ops_t` in the base image, supervisor hard-wired to SVPWM. Zero
   behavioral change; `platform_pwm_set*` signatures unchanged; validates the
   architecture. Gate: existing graphs emit, build, and run identically.
2. **Legacy-path cleanup** — point `FocControlManager` at the extrapolated
   encoder angle; retire the `foc_active`/`spwm_running` flag pair into the
   modulator state.
3. **SHEPWM engine on TIM5, Option B** — compare-chain ISR writing TIM1
   forced modes; ping-pong tables; standalone test on the **forced-angle
   ramp** at low bus voltage, current-limited supply, scope on phase voltage
   and current. Validate dead time at forced transitions.
4. **Live handoff SVPWM ⇄ SHE** — phase-locked `enter()`/`exit()`, toggled
   repeatedly at low voltage before raising bus; verify BKIN trip works
   mid-SHE (inject gate-driver fault, confirm outputs idle).
5. **Supervisor + hysteresis + KV transition table** — tune on hardware
   (dyno or bench motor); decide the MI-continuity strategy (§3) here.
6. **Optional:** DMA circular pattern feed; RCFM modulator.
7. **Codegen baking (last)** — once the base-image patterns are proven:
   supervisor surface as nodes/config, document the modulation-slot contract
   in `platform_api.h`, and only then consider a first-class
   component-lifecycle feature in `InverterCodegen`.

## 9. Open Questions to Resolve Before Starting Step 3

1. **MI continuity under SHE** — table family + interpolation vs hybrid trim
   vs saturation-region-only (§3). This decides the table format and the
   `enter()` contract.
2. **SHE operating region** — target fe range and pulse counts (the 35/40 Hz
   thresholds in the original plan are placeholders; at 40–300 Hz a 51-pulse
   pattern means 2–15 kHz effective switching — check device thermal limits,
   current default is 2.5 kHz carrier / 5 kHz update).
3. **She angle source** — generated offline (Python script into a C table, or
   FRAM KV blob)? Recommend offline solve → compile-time tables first, KV
   upload later.
4. **Where `commit()` runs for SHE** — TIM5 CC ISR at what NVIC priority
   relative to ADC (4) and TIM1_UP (5)? Must preempt the control ISRs to hold
   edge jitter, must not preempt the AWD/fault path.
5. **Whether the legacy `FocControlManager` path stays** after graph-based
   FOC reaches parity — affects how much of step 2 is worth doing.

## 10. Hard Prerequisite for Closed-Loop FOC + SHEPWM: Current Oversampling / Ripple Compensation

Bench testing showed that running FOC at low effective switching frequencies
(even with variable-carrier SVPWM, and certainly behind a fixed SHEPWM pattern)
produces unstable current feedback and can destroy hardware. The root cause is
not the modulator itself; it is the current measurement:

- The existing base image samples phase currents once per PWM period at the
  bottom (TIM1 CH4 TRGO → ADC1/ADC2 injected group). This is adequate at the
  default 2.5 kHz carrier / 5 kHz update, but degrades as the carrier drops or
  as the pattern becomes non-carrier-based.
- A single bottom sample does not represent the average phase current when
  ripple is large, and the sampled value becomes a function of duty cycle and
  electrical angle.
- Closing a current loop on that biased, ripple-dependent feedback leads to
  instability and overcurrent trips / device failure.

**Therefore, closed-loop FOC driving SHEPWM is gated on a separate foundation:
current oversampling and/or software ripple compensation.** Options:

1. **Hardware or triggered multi-sample averaging** — configure the ADC to
   capture several samples per PWM period (e.g., at multiple points in the
   switching cycle) and average them in software.
2. **Software ripple compensation** — model the inductor current ripple from
   the applied voltage vector, inductance, and back-EMF, then offset the
   sampled value toward the average.
3. **Hybrid** — oversample to reduce noise, then apply a model-based correction
   for the residual.

Until this is validated against a high-bandwidth current reference across the
intended carrier/frequency envelope, **SHEPWM must remain a voltage-source
pattern only**: open-loop, handoff target, or supervisor-driven at speeds where
FOC is no longer required. Do not attempt to close FOC around SHEPWM without
first solving current measurement.

This gates step 4 (live FOC ⇄ pattern handoff under load) and step 5
(supervisor + closed-loop operation) for SHEPWM specifically. The modulator
refactor, SVPWM/SPWM work, and manual pattern shell commands can proceed
independently.
