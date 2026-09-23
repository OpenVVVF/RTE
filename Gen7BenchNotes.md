# Gen7 Bench Notes — handoff (2026-09-22 session)

State of the Gen7 inverter + Zero IPM motor on the dyno. Read this before
touching the rig; it exists so the next agent does not re-derive three days of
debugging. Bench hardware facts and measured numbers below are ground truth;
theories labeled DEAD are disproven — do not resurrect them without new
hardware facts.

## Operating the rig (all via RTE MCP tools)

- Repo `/home/tliao/Desktop/RTE`, branch `coproc-flash-fixes`.
- Run motor: `control start` / `control stop`, torque via `var set IqVar <A>`
  (positive = encoder-positive direction). Legacy `foc` command is dead.
- **Faults (NEW firmware, 2026-09-22 eve):** the old latch bug is FIXED. Faults
  block `control start` only while Critical/High faults are active; recover
  with `fault clear high` + `fault clear warning` (or per-source). Validated
  end-to-end with injected test faults — no reboot needed. **Trap:** bare
  `fault clear`/`fault clear all`/`clearfault` refuses at IDLE ("stop
  control/PWM first") because MOE is now always set — the `all` scope hits
  the gate-reset path's power-stage check. Use scoped clears until fixed.
  `fault status` / `fault_flags_hex` / `fault_active_names` show live state;
  `reboot` still works as a sledgehammer.
- **Config trap:** `config set <key> <val>` is LIVE-ONLY for graph-node keys
  (wiped on reboot) unless followed by `config save <key>`. KV keys persist
  immediately. `var set` values reset on `control stop`.
- After any reboot, FRAM-loaded config applies automatically (see table).
- Build/flash: `rte_build(graph="Assets/Examples/foc_demo.json",
  base_source="Images/Gen7FW/MainProcessor")` → flash the ELF from the
  reported artifacts path, then `reboot`. If the MCP `rte_build`/`rte_flash`
  actions return a bare "operation failed", use the CLI fallback
  (`build/bin/rte build ...` / `build/bin/rte flash --firmware <elf> --target
  main`) — same session, same result.
- Telemetry is 128 Hz and aliases PWM ripple; trust the 5 kHz spike capture
  (`spikes <A>`) and `enc_trace` for waveform truth. The DC-link power signal
  (`dclink_p_w`) is noisy and its current sensor has ~1.4 A offset —
  `dclzero` zeroes it (drive idle); prefer the bench PSU display for power.

## Working configuration (saved in FRAM — boots into this)

| Key | Value | Note |
|---|---|---|
| Motor.Encoder.SinCos.OffsetDeg | 150 | hand-tuned; cal value was 165.7 (see open items) |
| Ctrl.PiQ.Kp / Ctrl.PiD.Kp | 0.04 | stock 0.08 ran away above ~300 RPM @ 50 A |
| Ctrl.PiQ.Ki / Ctrl.PiD.Ki | 5 | stock 10 |
| Motor.Lambda | 0.04 Wb | back-solved from vq at 25 A/350 RPM; 0.08 over-injects 2x |
| Motor.Ld / Motor.Lq | 0 / 0 | cross-coupling FF DISABLED (see FF runaway below) |
| Motor.KneeV | 0 | q-axis IGBT-knee comp off by default |
| Motor.Poles=10, R_phase=12.1 mOhm, salient IPM Ld=93.6 uH, Lq=253 uH | | from cal (FRAM) |

Graph defaults match (foc_demo.json): feedforward node wired, cross-terms 0.

## Achieved performance

- 65 A forward: 753 RPM stable (was: runaway at ~312 RPM with stock gains).
- 25 A @ offset 150: 752 RPM, clean. ~2x torque-per-amp vs cal offset.
- Loaded/regen with dyno holding 500 RPM: ±10..±60 A braking BOTH directions,
  loops clean (id ±2..6 A), regen into Sorensen PSU (160 W @ 60 A in the
  encoder-positive direction), IGBTs <39 C, bus steady.
- Encoder stream glitch-free since firmware fix (`enc_rejects` = 0 all day).

## Fixed this session (local commit 55d4c0d, NOT pushed)

1. **EncoderADC outlier rejection** (EncoderADC.cpp/h): single-sample angle
   outliers (>10 deg/sample = physically impossible) are rejected before they
   touch the snapshot, learned bounds, fit accumulator, or trace. Bounds only
   learn from validated samples (EMI bursts used to poison bounds and
   permanently shift the decoded angle — measured +127 deg retained slips).
   3-sample holdoff accepts genuine steps. `enc_trace` now snapshots the ring
   under IRQ lock (dump no longer races the writer). New telemetry:
   `enc_rejects` (1 Hz).
2. **Knee compensation** (graph FocFeedforward node): IGBT/diode conduction
   knee term added, **q-axis only** with 0.5 A deadband. The d-axis sign()
   relay-chattered on noise around id~0 and injected +/-Knee V square waves
   (audible crackle, id spikes past -75 A). KneeV defaults 0.
3. **FocFeedforward re-added** to foc_demo.json (was stripped in bdb2960):
   wired to Park I_D/I_Q, RpmElec, PiD/PiQ Feedforward ports, fed by
   CfgLd/CfgLq/CfgLambda/CfgKnee (new Motor.KneeV key).
4. **CfgLambda default 0.04** (measured). **Loop-gain defaults 0.04/5**.
5. Known firmware follow-ups still open (as of merged fw, graph hash
   `d80ff3057ad74897`): `fault clear all` refuses at idle (MOE check — use
   scoped clears); MAX22530 `clearFilter(3)` reports FAILED during clear;
   flux-cal stage saves nothing; DC-link current sensor offset (~1.4 A).

## OPEN MYSTERIES (ranked) — and dead theories

### 1. Direction-dependent torque/alignment (the big one)
- **Symptoms:** optimal offset differs by direction (fwd ~150 @ 25 A; rev ~120
  @ -10 A — comparison partly confounded, see below). At 60 A dyno-held
  regen: encoder+ direction returns ~160 W to PSU; encoder- direction PSU
  *draws* 20-50 W (Sorensen display, separate supplies — net accounting ruled
  out). Reverse unloaded free-spins to the 1350 RPM voltage ceiling on ~14 A;
  forward is load-limited.
- **DEAD theories (do not re-litigate):** shaft windup/compliance (encoder is
  bolted to the Zero motor assembly — rigid, user-confirmed); encoder
  nonlinearity (`encinl` 2026-09-22: harmonics all <=0.12 deg, H02=0.05 deg —
  decode is excellent; residual rms 1.7 deg/pp 11 deg is per-sample measurement
  noise; real direction-dependent lag only ~2.1 deg rms — too small); encoder
  cable EMI (moved cable 1 ft: matched A/B point unchanged, rev -10 A @150 =
  407 vs 394 RPM pre-move); load-dependent angle (angle source cannot see
  current).
- **Live suspects:** current feedback path (measured id/iq errors — transducer
  gains/Clarke), or the offset-split observations were confounded by the
  >800 RPM unstable zone (the 25 A forward sweep that "peaked at 150" tripped
  at 145/900 RPM before finishing).
- **Next probes, in order:**
  1. **vd readout** (cheapest, decisive): at perfect alignment vd must equal
     -we*Lq*iq exactly. Compare measured vd at matched operating points in
     both directions; the deviation IS the frame error, no power math.
  2. **15-20 A regen A/B both directions** with dyno at 500 RPM + Sorensen
     display (losses only ~50-80 W at that current, so net reading ≈ gross).
  3. Matched offset sweeps at 15 A (safe, below runaway zone) both directions.
  4. Only if 1-3 point at the sensor: `encinl` rerun + `encfit`.

### 2. Unloaded high-speed instability (~800-900 RPM ceiling)
- Any current above ~25-30 A (offset 150) accelerates into the 800-900 RPM
  zone and PhaseOvercurrent-trips; ripple grows with current below that.
  Gain-insensitive (halving gains moved the wall from ~300 to ~800, nothing
  more). NOT voltage-limited (18 V back-EMF vs 28 V available at 800 RPM).
- **Feed-forward runaway:** enabling the Ld/Lq cross-coupling FF terms at ANY
  speed/gain causes instant/current-proportional runaway (faulted in <1 s at
  753 RPM/65 A when Ld was set live). This is why Ld/Lq default 0. The FF
  runaway and the unloaded ceiling are probably the same mechanism; both
  remain unexplained. Cross-FF is wired in the graph but zeroed for safety.

### 3. 49 V physics wall
- Back-EMF rectifies to bus at ~1350 RPM (observed ceiling). Above that,
  diodes conduct uncontrolled. Controlled operation (either sign) must stay
  below ~1200 RPM at 49 V. Bus raise to 100-150 V is the agreed next
  hardware step (user will do; 1700 V IGBTs, knee becomes ~1.5 %).

## Bench facts
- Real bus ~49-50 V (Sorensen bidir PSU, sinks regen fine). Phase overcurrent
  trips latch as Critical faults — recover with `fault clear critical` (or
  `high`+`warning`), NOT `fault clear all` (idle-MOE refusal, see above).
  hwoc/spike recorder available: `spikes <A>`.
- Motor: 10-pole salient IPM (Zero), R 12.1 mOhm, Ld 93.6 uH, Lq 253 uH,
  lambda ~0.04 Wb. Encoder: 1-cycle sin/cos, rigid mount, bounds
  sin 11354..53904 / cos 11343..53798 (learned, healthy).
- Dyno: ABB drive, 480 V 3-phase, SEPARATE circuit from the Sorensen.
  Direction naming: dyno-"forward" = encoder-negative (Mech_RPM -), our
  offset-150 "bad" direction; dyno-"reverse" = encoder-positive.
- `encinl start 10 1` works but needs the cal-convention offset (165.7) —
  with 150 it overcurrents in FIND_VOLTAGE (cal applies voltage in its own
  frame). ~15 min runtime, rotates the motor slowly.

## Misc notes
- `Motor.Encoder.SinCos.OffsetDeg` 150 was tuned for unloaded forward
  operation; loaded 500 RPM braking looked clean at 150 in BOTH directions
  (id small) — the loaded/unloaded behavior differs, which is itself a clue.
- Every config change mid-run is a live experiment: prefer set-before-start
  after a reboot for clean comparisons; changing offset at speed kicks the
  loop (frame rotates by delta instantly).
- Tree state: colleague's fault-clear/telemetry-ISR changes merged into
  `coproc-flash-fixes` (clean merge) and reflashed 2026-09-22 evening. Host
  tools rebuilt + ctest subset green. Branch is ahead of origin (merge commit
  + local commits) — user holds push approval.
