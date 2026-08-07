# FPGA FOC Fundamentals — what we're building and why

**Audience:** the developer (you) who wants to understand, not just ship.
**Scope:** the hybrid Nucleo-L476 (MCU) + Tang Nano 20K (FPGA) control platform.
Read this alongside `docs/ROADMAP_HYBRID_NUCLEO_FPGA.md` (the plan) and
`Fpga/TangNano20k/docs/register_map.md` (the contract).

---

## 1. The problem we're solving

A permanent-magnet synchronous motor (PMSM) is driven by three phase voltages,
120° apart. To make smooth torque you must continuously decide, tens of
thousands of times per second, what voltage vector to apply — based on currents
you measured a microsecond ago and the rotor's electrical angle *right now*.

That decision loop is **field-oriented control (FOC)**, and it has two very
different kinds of work in it:

| Kind of work | Examples | Tolerance for delay/jitter |
|---|---|---|
| **Hard real-time, bit-exact** | PWM edge placement, dead-time, Clarke/Park transforms, SVPWM duty math, over-current latch | Nanoseconds to microseconds. A late PWM edge = distorted current = torque ripple, noise, heat. A missed over-current event = dead MOSFETs. |
| **Soft real-time, flexible** | Speed loop, mode logic, telemetry, calibration, FRAM config, safety supervision, parameter tuning | Milliseconds are fine. Complexity and changeability matter more than determinism. |

A CPU *can* do the first kind (the Gen6 firmware does), but it spends a large
fraction of its interrupt budget on it, and every cache miss or priority
inversion shows up as current distortion. An FPGA does the first kind
*trivially* — it is literally made of the counters and comparators this work
needs, and its timing is exact by construction.

So the hybrid split is not about making things faster for its own sake. It's
about putting each kind of work on the substrate that is naturally good at it.

## 2. FOC in one page

The trick of FOC is to turn a hard 3-phase AC problem into an easy 2-channel DC
problem:

1. **Measure** two phase currents `ia`, `ib` (the third is implied: `ia+ib+ic=0`).
2. **Clarke transform** — project the 3-phase quantities onto a stationary
   2-axis frame `(α, β)`. Still AC (sinusoids at electrical frequency).
3. **Park transform** — rotate `(α, β)` by the rotor's electrical angle `θ`
   into the rotor's frame `(d, q)`. In this frame, steady-state currents are
   **DC values**:
   - `id` — flux-producing current (usually commanded to ~0 or negative for
     field weakening),
   - `iq` — torque-producing current. **Torque ≈ Kt · iq.** This is the whole
     point: commanding torque becomes commanding one DC number.
4. **PI current loops** — two ordinary PI controllers steer `id`, `iq` to their
   setpoints and output desired voltages `vd`, `vq`.
5. **Inverse Park** — rotate `(vd, vq)` back by `-θ` to `(vα, vβ)`.
6. **SVPWM** (space-vector PWM) — figure out which inverter switches to turn on,
   for how long, within one PWM period to synthesize that voltage vector.
   Produces three duty cycles `du, dv, dw` in 0..100%.
7. **Gate drive** — turn duties into six gate signals (high/low side per phase)
   with **dead-time** inserted: the high and low side of a leg must never be on
   simultaneously ("shoot-through" = short across the DC bus = fireworks), so a
   small delay (~100 ns–1 µs) is enforced between one turning off and the other
   turning on.

Steps 2–3 and 5–7 are pure deterministic math on recent samples — ideal FPGA
work. Step 4 can live on either side; our roadmap puts the PI loops (or LADRC)
on the MCU initially because that's where tuning and telemetry live, with the
option to harden them into the FPGA later.

## 3. Q15 fixed-point — why and what it means

The Tang Nano's GW2AR FPGA has no floating-point unit. Floating point in RTL is
possible but big and slow. Instead we use **Q15 fixed-point**: a signed 16-bit
number interpreted as a fraction in the range **[-1.0, +0.99997]**, where the
binary point sits after the sign bit:

```
real_value = raw_integer / 32768
0x7FFF = +0.99997 ≈ +1   0x0000 = 0   0x8000 = -1.0
```

Rules of the game:
- **Multiplication:** Q15 × Q15 = Q30, so you take the top 16 bits of the 32-bit
  product (arithmetic shift right by 15) to get back to Q15. Verilog:
  `(a * b) >>> 15`.
- **Saturation:** intermediate results can exceed ±1 (e.g. PI integrator windup,
  over-modulation). Every accumulation stage must *saturate*, not wrap —
  wrapping a current command from +0.9 to −1.0 would command full reverse
  torque for one cycle. The RTL uses explicit clamp logic for this.
- **Scaling is a design decision:** currents are normalized by a full-scale
  value (e.g. ±50 A → ±1.0), angles by π or 2π. The scaling constants are part
  of the MCU↔FPGA contract — if the MCU thinks ±1.0 = 50 A and the FPGA thinks
  it's 100 A, you get exactly 2× torque error. Document them in the register
  map, not in tribal knowledge.

## 4. The split of duties — who does what

This is the architectural heart of the project. The guiding principle:
**the FPGA owns everything that must happen on PWM-period boundaries; the MCU
owns everything else.**

### FPGA (Tang Nano 20K) — the "power stage brain"

Runs off its own oscillator (27 MHz on the Tang Nano 20K). Cycle-exact, no
interrupts, no jitter.

- SPI slave register file (the only MCU-facing surface)
- Current-loop transforms: Clarke, Park (needs sin/cos of θ)
- SVPWM: voltage vector → three duties
- PWM generation: center-aligned counters, six outputs
- Dead-time insertion per leg
- Fault latching (over-current, over-voltage, bad config) — must be able to
  force all gates low *without MCU involvement*, because the MCU may be the
  thing that just crashed
- (Later) ADC interface for phase currents, QEP/encoder decoder for θ

### MCU (Nucleo-L476) — the "system brain"

Runs the RTE-generated node graph — everything that benefits from being
inspectable, tunable, and reprogrammable from the GUI.

- Outer loops: speed PI / LADRC → `iq` setpoint; field weakening → `id` setpoint
- Sends setpoints + feedback (measured currents, θ) to the FPGA over SPI
- Telemetry (IVP over UART/CAN) → NodeGUI scope plots
- Calibration routines, FRAM KV config, safety state machine
- Watches the FPGA's FAULT register; decides recovery policy

### The contract between them: SPI register map

The *only* coupling is the frozen register map (24-bit frames: addr + 16-bit
data, Mode 0, ≤10 MHz). Think about the bandwidth budget honestly:

- At a 10 kHz inner loop you have 100 µs per cycle. Writing ~6 registers
  (2 setpoints + 3 feedback + housekeeping) = 6 × 24 bits = 144 bits ≈
  14.4 µs at 10 MHz. Comfortable. If we later move PI loops into the FPGA,
  the MCU traffic drops to setpoints-only (~2 registers) per cycle.
- **Clock-domain crossing (CDC)** is the classic FPGA gotcha here: SPI bits
  arrive on the MCU's clock; the PWM logic runs on the FPGA's. Values crossing
  between them must be synchronized (double-flop synchronizers) and multi-bit
  values must be latched atomically (update a shadow register, commit on
  chip-select rising edge) — otherwise the SVPWM can read a half-updated
  duty value where the high and low bytes come from different cycles.
  `spi_regs.v` handles this; understand it before touching it.

## 5. Module-by-module: what exists, how it works, what's missing

All RTL lives in `Fpga/TangNano20k/rtl/`, testbenches in `Fpga/TangNano20k/tb/`.

### 5.1 `sincos_lut.v` — sine/cosine lookup ✅ implemented

Park and inverse-Park need sin(θ) and cos(θ) every cycle. Computing them in
RTL directly (CORDIC) costs latency and area; a lookup table is the standard
answer. The module stores one quadrant of a sine wave (0–90°) in a small ROM
and derives the other three quadrants from symmetry; cosine is just sine
shifted by 90°. Things to learn when you read it: how the quadrant folding
works (bit tricks on the angle's top bits), and how angular resolution
(θ is 16-bit, table has fewer entries) is traded against table size —
linear interpolation between entries buys back accuracy almost for free.
**Verify with:** `tb/tb_foc.v` sweeps θ and compares against expected values.

### 5.2 `foc_clarke_stub.v` — Clarke transform ✅ implemented (rename pending)

`iα = ia;  iβ = (ia + 2·ib) / √3`. Trivial math, but it's the reference example
of Q15 care: the `1/√3` constant is itself stored as Q15 (0.57735 × 32768 ≈
18919), and the sum `ia + 2·ib` needs 17 bits before scaling — an early
saturation lesson. (Files are named `_stub` for historical reasons; Phase 1
renames them.)

### 5.3 `foc_park_stub.v` — Park transform ✅ implemented (rename pending)

`id =  iα·cosθ + iβ·sinθ;  iq = −iα·sinθ + iβ·cosθ`. Four Q15 multiplies and
two add/subtracts, plus the inverse rotation for the voltage path. Read it
side-by-side with `sincos_lut.v` to see how multi-cycle pipelining is done:
the LUT takes a cycle, the multipliers take a cycle, the adds take a cycle —
the module presents a fixed-latency pipeline rather than a combinational blob,
which is what lets it meet timing at tens of MHz.

### 5.4 `foc_svpwm_stub.v` — space-vector PWM ✅ implemented (rename pending)

The most subtle block. Given `(vα, vβ)`: determine which of the 6 sectors of
the hexagon the vector falls in, compute how long to activate each of the two
adjacent active vectors plus the zero vectors, and map that to three phase
duties. Also responsible for **saturation**: if the requested vector exceeds
the inscribed circle of the DC-bus hexagon, it must be limited (linear
modulation) or allowed into over-modulation/six-step — the firmware does this
in floating point today; compare `Transforms.Svpwm` template's approach with
the RTL's.

### 5.5 `pwm_complementary.v` + `deadtime_pair.v` — gate output stage ✅ implemented

Center-aligned PWM: a counter ramps 0→N→0; each channel compares the duty
against the counter. Center alignment matters because it makes all three
phases' switching edges symmetric around the period center — which is also
where you sample currents (quiet point, away from switching noise).
`deadtime_pair.v` takes one "ideal" leg signal and emits high/low gate signals
guaranteed non-overlapping by DEADTIME_NS. This is the last line of defense —
even if every upstream block goes insane, shoot-through is impossible as long
as this block works. That's why it's deliberately tiny and dumb.

### 5.6 `spi_regs.v` — register file / MCU interface ✅ implemented

The 24-bit frame decoder (shift register), address decode, and the register
array. Holds the CDC discipline described in §4. `tb/tb_spi.v` verifies the
framing against `register_map.md`.

### 5.7 `tangnano20k_top.v` — integration ⚠️ v1 only

Today it wires registers → raw duties → PWM. **Phase 1 adds the FOC path:**
a mode mux (`CTRL.FOC_EN`) selecting between register-written duties (v1,
keep for bring-up!) and engine-computed duties, plus the new FOC setpoint/
feedback registers from register map v2. Keep the raw-duty path forever —
it's your hardware checkout tool: before you trust the engine, you prove the
SPI link and the power stage with manually commanded duties.

### 5.8 What's missing entirely (the real remaining work)

- **Register map v2** — FOC mode bit, Id/Iq setpoints, current/angle feedback
  registers, engine status/fault bits, version bump. Design first, freeze, then
  implement against it.
- **Current feedback path** — v2 assumes the MCU measures phase currents (its
  ADCs) and writes them to the FPGA. Fine to start. Longer term the FPGA can
  own the ADC interface (hall/current sensors via SPI ADC) for lower latency.
- **Angle source** — same story: MCU computes θ from encoder/Hall and writes
  `THETA_EL`. Later: QEP decoder in RTL (it's ~50 lines — a counter with
  quadrature edge logic — and a great learning module).
- **Hardware over-current protection** — comparators on the ADC inputs (or a
  digital threshold on written feedback) that latch FAULT and kill gates
  without software. Non-negotiable before spinning a real motor with real power.
- **Real MCU SPI driver** — `FpgaSpiDriver.cpp` in the Nucleo image currently
  returns *simulated* register values (audit flagged it: `FpgaSpi_Init`
  reports success with no hardware attached). Phase 2 replaces the stub with
  actual HAL SPI2 transfers matching the 24-bit framing.

## 6. Verification workflow (how you learn without burning hardware)

1. **Unit sims (iverilog):** `scripts/build_sim.ps1|.sh` runs the testbenches.
   Write the expected values by hand or from a small C/Python reference — the
   point is *you* predicting what the hardware should do, then being right or
   wrong. This is where the learning happens.
2. **Whole-engine sim:** `tb/tb_foc.v` — feed a rotating θ and known currents,
   check duties against the firmware's float SVPWM for the same inputs.
   Discrepancies are almost always scaling or saturation — finding them here is
   100× cheaper than on a scope.
3. **Synthesis:** Gowin IDE via `gowin_project.tcl` (or `gw_sh` CLI). Watch
   utilization and the timing report — a design that fails timing can
   *simulate* perfectly and *fail* on hardware.
4. **Staged bring-up** (the checklist in the roadmap): MAGIC probe → SCRATCH
   loopback (proves SPI) → raw duties on a scope with **no motor** (proves
   PWM + dead-time — measure the dead-time gap!) → low-voltage motor spin in
   raw-duty V/f → FOC mode open-loop → closed loop. Never skip a stage; each
   one isolates exactly one new thing that can be wrong.

## 7. Suggested learning order

1. Read `register_map.md` then `spi_regs.v` until you can predict what each
   frame does. Modify SCRATCH, re-run `tb_spi.v`.
2. Read `pwm_complementary.v` + `deadtime_pair.v`; run `tb_pwm.v`; change
   DEADTIME_NS and watch the waveforms (dump VCD, open in GTKWave).
3. Work through the Q15 math in `foc_clarke`/`foc_park` with a calculator;
   extend `tb_foc.v` with a case you computed by hand.
4. Design register map v2 on paper (what addresses, what units, what happens
   on bad writes) — bring it to review before writing RTL.
5. Only then touch `tangnano20k_top.v` integration.

The order is deliberate: interface → output stage → math blocks → contract →
integration. Each step uses only the knowledge from the previous ones.
