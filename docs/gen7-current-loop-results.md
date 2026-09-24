# Gen7 current-loop repair: 75 V results

Bench session: September 22–23, 2026. Final motor state: stationary, IqVar=0,
IdVar=0, generated outputs disabled, PWM MOE=0, gate ready, no fault flags.

## What was fixed

- Boot failure after flashing: FRAM loaders logged before the NOLOAD telemetry
  queues were initialized. Initializing telemetry first fixed the reproduced
  hard fault. The final image booted after flashing and after a shell reboot.
- Current and encoder snapshots are coherent across interrupt preemption;
  nested critical sections preserve the outer interrupt state.
- One controller limits the combined physical d/q voltage vector and feeds
  that final limit back to both integrators. Modulation uses physical volts.
  An explicit 0.5 compatibility factor retains the existing effective gains
  and feedforward settings; stored Kp/Ki were not doubled or retuned.
- Current acquisition now samples both PWM extrema, at 5 kHz during control
  with 2.5 kHz switching. Each sample is transformed using its own rotor angle.
  The controller uses a symmetric full-cycle estimate:
  `0.25*dq[n] + 0.5*dq[n-1] + 0.25*dq[n-2]`.
  This adds one sample (200 us) of estimator delay. Missing, nonalternating or
  stale pairs are not combined. Raw current still drives overcurrent protection.
- Raw d/q remains visible as `cg_id_raw_a` / `cg_iq_raw_a`, and in `ctrlcap`.
  `cg_id_a` / `cg_iq_a` are the actual cycle-estimated controller feedback,
  not just smoothed display values. This does **not** prove that instantaneous
  winding-current ripple disappeared.
- Bounded 512-frame capture records real timing, raw and estimated d/q, raw
  phase currents, voltage requests/limits/integrals, and duty estimates.
  The spike recorder and host parser now use captured cycle timestamps and
  measured sample rate. No binary telemetry framing/protocol change was made.
- Telemetry queue handling no longer consumes string fragments when their
  destination queue or UART frame buffer lacks room.

The previous direction/phase mapping and neutral PWM startup fixes remain.
The experimental single-duty-update-per-cycle scheduling was **reverted**;
the final graph controller still runs at 5 kHz.

## Measurement that isolated a cause

At approximately +1,190 RPM and +20 A, changing only the selected PWM midpoint
reversed the dominant third-electrical-harmonic d-axis sample component:

| Sample point | Fitted RPM | Mean iq | Third-harmonic sine / cosine coefficients |
|---|---:|---:|---:|
| Bottom | 1188 | 20.10 A | -1.788 / +2.088 A |
| Top | 1198 | 20.11 A | +1.934 / -2.140 A |

This implicates a PWM-phase-dependent measurement component. It does not
distinguish actual switching ripple from analog sensor settling/pickup.
Single-midpoint sampling aliases this component into the apparent d/q waveform.
Acquiring both midpoints and estimating the cycle current reduces what the
controller reacts to. Encoder offset changes are not needed for this effect.

Additional voltage headroom produced unclipped +35/-35 A tracking while the
ripple persisted: voltage saturation alone was ruled out as its cause.

## Final measurements

75 V nominal bus; actual approximately 74.3 V. Commands ±35 A. Each final
waveform contains 512 samples at measured approximately 5,000 Hz. Speeds below
come from the slope of unwrapped captured encoder angle (five pole pairs),
not the slower dashboard RPM estimate. These are accelerating windows, not
exact matched steady-state dyno points.

| Metric | Positive | Negative |
|---|---:|---:|
| Fitted shaft RPM | +2161 | -2185 |
| Mean feedback iq | +35.49 A | -35.15 A |
| Mean feedback id | +1.31 A | +1.08 A |
| Feedback id AC RMS | 4.43 A | 4.17 A |
| Feedback iq AC RMS | 2.27 A | 2.29 A |
| Raw id AC RMS, same capture | 10.33 A | 10.21 A |
| Raw iq AC RMS, same capture | 4.04 A | 3.01 A |
| Minimum voltage limit scale | 1.000 | 1.000 |
| Minimum sample-valid flag | 1 | 1 |

Earlier single-midpoint captures with the same 0.45 voltage fraction, at nearby
+2185/-2260 RPM, had id AC RMS 11.73/11.51 A and iq AC RMS 3.59/2.87 A.
They also used the subsequently reverted single-update timing, so this is
not an isolated estimator-only A/B. The raw/estimated comparison within each
final capture is the cleaner estimator comparison.

Zero-current enable capture: largest absolute measured/reconstructed phase
current was 3.252 A across the recorded 102 ms. No former large enable spike
was reproduced. Powered comparison runs completed without a control trip.

The motor was stopped at approximately 2,050 dashboard RPM in the final runs,
leaving margin for its lag relative to instantaneous encoder-angle speed and
the user's 3,000 RPM ceiling. Full-speed/high-load stability is **not** established.
Remaining feedback ripple is measurable; this repair reduces it, not eliminates
every current oscillation. No external current-probe or torque-ripple validation
was performed. Do not interpret the estimator improvement as equivalent measured
loss, torque or winding-ripple improvement.

## Persistence and image

Saved and verified after reboot: `Ctrl.VoltLimitPu=0.45`.
Read back unchanged: offset 150 degrees, d/q Kp=0.04, d/q Ki=5.
The 0.45 setting provides 35% more voltage headroom than the old physical Vdc/3
ceiling. It is configurable, not an RPM limit; sampling-margin-limited maximum
utilization remains unvalidated. Graph fallback default remains Vdc/3.
The temporary `Ctrl.SampleAtTop` diagnostic key was removed. Spike threshold
restored to 50 A. No Ld/Lq, knee, polarity or pole-count tuning was performed.

Main MCU image: `build/current-loop-fw/STM32CubeMX.elf`.
Flashed ELF SHA256: `2031933c17d09ad6f86f1cb8d9064fd550739b73ac4949b7f2a5cb72a5b5f61b`.
Flashed BIN SHA256: `3da04bb62a891ff989020a0072c592c5166739e1b44697319b37597ff83a2aa6`.
Graph hash: `855dbcee90d611cb` (graph hash alone does not identify base-code changes).
Main MCU reflash required; no coprocessor reflash. Native `foc start` diagnostic
was not exercised after changing its physical-voltage API.

## Checks and artifacts

- Built current main firmware, `rte` and `RTEStudio`; 14 targeted controller,
  CLI, flash-integration and session-state tests passed. `git diff --check` clean.
- Connected firmware command catalog complete: 51/51; `ctrlcap` discovered;
  raw signals and graph manifest read successfully.
- Rebuilt CLI spike tool parsed 64 hardware samples at 4999.984 Hz with timestamps.
  The already-running MCP process still has its older tool description/parser;
  reload that process to use the rebuilt specialized tool through MCP. Generic
  commands work; the rebuilt `rte tool rte_spike_capture` route was verified.
- Existing Studio process labels some idle ISR values `control_stopped` despite
  recent samples and `tim_isr_running=1`; the rebuilt host tests cover corrected
  reporting. Final MOE/output/fault flags were checked directly. No unsaved Studio
  session was discarded to reload its executable.
- Occasional UART warning/command loss occurred during stopped-motor page export;
  missing pages were retried without rearming the frozen recorder. Warnings were
  cleared, and final fault flags were zero. This serial reliability issue remains.

Data and reproducible analysis are in `captures/direction_asymmetry/`:
`bus75_cycle35_{positive,negative}_ctrlcap.txt`, `bus75_cycle_startup_ctrlcap.txt`,
the corresponding host snapshots, `bus75_final_state.json`,
`bus75_spike_tool_check.json`, `bus75_current_summary.json`, and
`analyze_current_captures.py`. The original plan remains historical in
`docs/gen7-high-speed-current-fix-plan.md`.
