# Gen7 control-speed repair and fixed-frequency ripple trials

Date: 2026-09-24. Main graph `foc_demo`; switching stayed at **2,500 Hz**
throughout, with current/control updates at 5,000 Hz while running.

## Repair

The graph previously used `hw.rpm`'s dashboard estimate for voltage feedforward
and encoder angle lead. `EncoderADC::diagnose()` takes 20 ms speed windows and
applies an EMA with alpha 0.02: approximately one second of lag. During the
35 A acceleration capture, actual encoder speed was 2,077.9 RPM while the
feedforward calculation used 1,746.2 RPM.

Gen7 now estimates control speed from accepted encoder DMA samples: a 2 ms
angle-difference window followed by a 10 ms low-pass, using measured DWT cycle
intervals. Wraparound, missing samples and invalid values are handled explicitly.
Encoder age extrapolation uses this estimate as well. `Custom.ControlSpeed`
feeds the angle-lead and voltage-feedforward nodes; `cg_rpm_control` exposes
the speed actually supplied to those nodes. Existing dashboard RPM remains
unchanged. Gen6 API aliases preserve its previous behavior; HostSim aliases
use the exact plant speed. No simulator result was used for control decisions.

Only the main MCU was flashed. No binary protocol, command grammar, capture
layout, current-feedback filter, switching frequency or coprocessor change was
made. Rebuild/regenerate the main image from `Assets/Examples/foc_demo.json`;
the old tracked `.rte-gen7/generated` tree does not contain this repair.

## Bench measurements

75 V supply setting (measured approximately 74.4 V), free-spinning motor,
35 A iq and 0 A id. Captures contain 512 consecutive valid samples (~102 ms).
These are separate acceleration captures, not identical steady-state operating
points; the small ripple differences do not establish a repeatable improvement.

| Image/settings | Encoder RPM | Feedforward RPM | iq AC RMS / peak-to-peak, A | id AC RMS / peak-to-peak, A |
|---|---:|---:|---:|---:|
| `5b529e2` baseline, KpD/Q .04 | 2,077.9 | 1,746.2 | 1.805 / 11.297 | 3.534 / 18.518 |
| Speed fix, KpD/Q .04 | 2,152.9 | 2,149.1 | 1.726 / 9.954 | 3.460 / 18.241 |
| Speed fix, KpD/Q .08 | 2,104.7 | 2,100.9 | 1.757 / 10.109 | 3.414 / 16.282 |
| Speed fix, KpD .02 / KpQ .04 | 2,118.7 | 2,115.0 | 1.761 / 10.996 | 4.148 / 22.059 |
| Speed fix, KpD/Q .04, reverse | −2,138.1 | −2,134.0 | 1.701 / 9.279 | 3.332 / 19.367 |

Ki remained 5 on both axes. Feedforward RPM is reconstructed from the recorded
pre-limit q voltage, current error, integral and unchanged Lambda=.04, Ld/Lq=0,
Knee=0, with the existing 0.5 legacy-to-volts adapter. The encoder reference
speed comes from unwrapped angle versus measured capture time.

The fix removes the approximately 330 RPM acceleration lag. It does **not**
substantially eliminate current ripple. The positive speed-fix capture still
has raw iq 3.536 A AC RMS / 17.074 A peak-to-peak and raw id 9.984 A AC RMS /
41.455 A peak-to-peak. Existing 1/4, 1/2, 1/4 feedback reconstruction remains
correct; there were no sample-sequence gaps or voltage clipping in these runs.
No additional display smoothing was introduced.

The earlier 49 V baseline was materially noisier than the 75 V baseline and
had voltage clipping plus much smaller software-estimated distance to switching
edges. That remains a separate effect; see the baseline investigation.

## Rejected gain trial

KpD/Q .08 first passed a 10 A / 514 RPM check, then the 35 A capture above,
but did not provide a convincing improvement over .04. Raising both to .20
caused a growing current oscillation near 625 Hz around 400 RPM. Host monitoring
stopped graph control when a poll reported id −349 A, iq −70 A and bus 51 V.
The frozen spike recorder captured the onset. No fault remained latched after
the stop; the supply recovered to about 74.3 V. These are reported samples,
not bounds on the transient's absolute peak. The .20 gains were not saved.

The subsequent lower d-axis gain increased id ripple. Both gains were restored
to .04, and the reverse run above passed with those settings. Further gain
increases should not be inferred safe from the nominal motor inductances.
Timing/plant identification and direct current/voltage probing are appropriate
before trying to increase bandwidth again at this switching frequency.

## Final configuration and artifacts

- Graph control stopped, IqVar=IdVar=0, PWM off; final status checked separately.
- KpD/Q .04; KiD/Q 5. Original FRAM gain values were never overwritten.
- Runtime software phase overcurrent threshold 650 A. This `ocset` value is
  volatile: the existing firmware default remains 500 A after reboot.
- `Ctrl.VoltLimitPu` .54 remains saved, below the existing sampling-guarded
  linear ceiling (~.5427 at 2.5 kHz). This test did not approach that ceiling
  at 75 V or validate maximum-speed/full-voltage operation.
- Encoder offset 150°, sign +1, 10 poles and phase map 4 unchanged.
- Main graph hash `bd5390d76c425eb1`, 82 nodes / 33 declared signals.
- Main ELF: `build/control-speed-fw/STM32CubeMX.elf`.
- ELF SHA-256: `84a1b399b19feae7f46f988ded6b1bf63df339e57e87011095bf428a8fe225f4`.
- BIN SHA-256: `d07857dda5ae450bf02e6708a1f3ecd0e5ceb88f664cb15f4d18491d1685ad88`.

Current `rte`, `RTEStudio`, firmware and HostSim compile successfully.
All 18 targeted tests pass, including estimator acceleration in both directions,
timestamp/angle wrap, sample noise, gaps, irregular rates, current-loop math,
MCP integration and stopped-signal handling. Live command discovery, build
identity, signal discovery and telemetry verified the flashed image and new
signal. Host simulation was compiled only, not used as hardware validation.

Captures, command/monitor logs and the rejected trial are in
[`captures/baseline_5b529e2_20260924`](../captures/baseline_5b529e2_20260924/).
Run `python3 captures/baseline_5b529e2_20260924/analyze_speed_fix.py` to regenerate
the numeric summary and [comparison plot](../captures/baseline_5b529e2_20260924/speed_fix_comparison.png).
