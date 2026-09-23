# Gen7 direction asymmetry and startup-current fixes

Date: 2026-09-22. Bench: Gen7 inverter, Zero 10-pole IPM motor, ABB dyno,
Sorensen approximately 49–50 V. Main control is the RTE `foc_demo` graph.

## Result

The decisive fault was a **60 electrical degree mismatch between current
feedback coordinates and PWM voltage coordinates**, caused by the graph's
current phase order and polarity. Correcting that mapping made the matched
positive and negative acceleration tests approximately symmetric.

A separate startup defect applied stale PWM duties and enabled the bridge
phases sequentially. Neutral duty preload and simultaneous phase enabling
removed the large recorded startup spike.

Both fixes are in the main MCU firmware. No coprocessor update is required.
Commands, telemetry names, and MCP tool schemas are unchanged.

## 1. How the current-mapping fault was isolated

With the dyno free and the rotor allowed to settle, a diagnostic temporarily
set the graph pole multiplier and back-EMF feedforward to zero. That held a
fixed stator field independently of the encoder. The graph commanded
`IdVar = 15 A`, `IqVar = 0 A`, at three field angles. All temporary settings
were restored afterward; this used graph control, not native diagnostic FOC.

At essentially zero speed, the old mapping required the following voltage
commands to maintain the reported d-current:

| Fixed field angle | vd before, V | vq before, V | vd after, V | vq after, V |
|---:|---:|---:|---:|---:|
| 0° | 1.141 | 1.976 | 2.306 | −0.0077 |
| 60° | 1.122 | 2.007 | 2.306 | +0.0029 |
| 120° | 1.160 | 2.012 | 2.289 | +0.0120 |

`atan2(vq, vd)` was approximately +60° at every field angle before the fix.
The motor was stationary, so speed-dependent encoder lag and back-EMF did
not explain that rotation. After correcting the current mapping, the voltage
was almost entirely on the d-axis, as expected for settled d-current.
The angle comparison does not depend on the common voltage-command scale.

### Exact mapping change

In `Assets/Examples/foc_demo.json`, node type `Custom.PhaseCurrentsBurst`,
let `u` and `v` be the two current values **after the existing driver remap**,
and `w = -(u + v)`.

```text
Old graph: PWM-frame currents (A, B, C) = (-u, -v, -w)
Fixed graph: PWM-frame currents (A, B, C) = (v, w, u)
```

Equivalently, the corrected vector is `(-old_B, -old_C, -old_A)`.
The bench measurements identify the graph's first measured input as PWM
phase C and its second as PWM phase A; phase B is reconstructed.
This mapping was verified with **Motor.PhaseSwap = 4** already active in the
driver. It is not an instruction to apply that permutation directly to
unmapped ADC ranks or to arbitrary motor wiring.

A shared encoder-offset adjustment rotates the current and voltage frames
together; it cannot repair their relative 60° mismatch. With the old mapping,
reported zero d-current did not imply actual zero d-current. Unintended flux
current and saliency torque provide a mechanism for different behavior when
the current direction reverses. The stationary measurements isolate the
mapping error, and the matched acceleration tests verify its practical effect.

## 2. Additional feedback-timing corrections

- `PhaseCurrentADC.cpp`: publish the just-completed JDR burst before executing
  the generated ADC graph. Previously it consumed the preceding PWM period's
  samples, adding approximately 400 µs at 2.5 kHz.
- The ADC graph uses the actual PWM-period time step. ADC and TIM1 dispatch
  save and restore the interrupted domain's time step, so ADC preemption
  cannot change the current PI integration interval.
- Clarke and Park current transforms execute in the ADC domain with the
  encoder angle read during that callback. Inverse Park retains the separate
  actuation-angle lead. Measurement and actuation share one live set of
  encoder offset, sign, and pole-count settings through graph bridges.
- `PWM_FindSafeSamplePoint()` samples near the center of a complete symmetric
  zero-voltage window. It prefers the bottom window when valid, avoiding
  top/bottom switching caused by rounding of nearly equal window lengths.
  The old calculation selected the midpoint of only half the window.

These corrections improved timing and stability, but did not alone remove
the acceleration asymmetry. The stationary-field experiment identified the
remaining phase-mapping fault. PI gains stayed at Kp 0.04 / Ki 5 on both axes.

## 3. Startup-current correction

Resetting the graph cleared PI state but left the previous powered voltage
vector in TIM1's compare registers. Because the timer keeps running for
measurement while idle, graph reset alone did not establish neutral hardware
duties before the next start.

`ControlSupervisor::start()` now writes 50% on all three phases and transfers
those values into the timer before enabling the bridge. Graph actuator
writes are enabled after PWM startup.

The old `PWM_Start()` called the HAL start routines phase by phase; those
calls each asserted the main output enable. It now arms all three
complementary phase pairs with MOE clear, then enables them together. HAL
channel state is maintained, and hardware break protection remains active.

Recorded evidence:

- Before neutral preload: **111 A** phase-current peak on a zero-current restart.
- With neutral preload but sequential enabling: one later restart recorded **8.9 A**.
- With neutral preload and simultaneous enabling: **three consecutive
  restarts produced no capture above an 8 A trigger**.

This is a result at the recorder's sample instants, not a bound on every
sub-sample switching transient. Its displayed 5 kHz rate was previously
found inconsistent with the approximately 2.5 kHz ADC rate; no frequency
analysis based on that label was used here.

## 4. Final matched spin validation

Same graph settings, offset 150°, Id request 0 A, and ±35 A Iq requests.
Both runs started near rest and stopped after sustained graph voltage-vector
limiting. These are reported encoder speeds, not independently calibrated RPM.

| Measurement | Encoder-positive | Encoder-negative |
|---|---:|---:|
| Time to 1000 RPM | 3.519 s | 3.395 s |
| Acceleration through 400–800 RPM | 402.6 RPM/s | 413.0 RPM/s |
| Last powered speed after sustained limiting | +1292 RPM | −1281 RPM |
| Maximum logged absolute speed including stopping | 1310 RPM | 1310 RPM |
| Firmware fault | None | None |

The acceleration difference in that band is approximately 2.6%. An earlier
pair with the corrected current mapping gave 397.1 versus 407.0 RPM/s and
3.456 versus 3.443 seconds to 1000 RPM, supporting repeatability.
Crossing times are interpolated from approximately 6 Hz host polling.

## 5. Working persistent configuration and defaults

The 150° working offset was already surviving reflashes during diagnosis.
The source graph nevertheless retained an obsolete **6.445° fallback**.
The graph's offset fallbacks are now **150°**, and the PI nodes' unwired
fallback gains now agree with the tested live gains.

| Setting | Working value |
|---|---:|
| Motor.Encoder.SinCos.OffsetDeg | 150 electrical degrees |
| Motor.Encoder.SinCos.Sign | +1 |
| Motor.Poles | 10 |
| Motor.PhaseSwap | 4 — existing persistent driver remap |
| Ctrl.PiD.Kp / Ctrl.PiQ.Kp | 0.04 / 0.04 |
| Ctrl.PiD.Ki / Ctrl.PiQ.Ki | 5 / 5 |
| Ctrl.Iq.SlewAps | 50 A/s |
| Ctrl.ThrottleEnable | 1 |
| Motor.Ld / Motor.Lq | 0 / 0 — cross-coupling feedforward disabled |
| Motor.Lambda | 0.04 |
| Motor.KneeV | 0 |

The zero Ld/Lq settings here disable feedforward; they do not replace the
measured motor inductances of 93.6 µH and 253 µH. Current mapping is a graph
code change, and startup sequencing is a base-firmware change; saving FRAM
alone does not install either fix. Existing sensor calibration is preserved.

Thirteen graph configuration keys were explicitly saved with per-key FRAM
success responses. `Motor.PhaseSwap = 4` was separately read from the
persistent store. After reflashing and rebooting the main MCU, **all 14 values
were read back and matched**, before issuing any live configuration updates.
The running graph hash and built artifact both report `dd8a7cd4d92a99c9`.
The binary SHA-256 is
`36c6ed503e19da80a3d9aa08592d2e63ac34791c4c55941c783b1fb69746c640`.
Save confirmations and reboot readbacks are retained in
`captures/direction_asymmetry/configuration_persistence.json`.

Current requests default to zero and control starts idle. Use `control start`
for the main graph and `var set IqVar <A>` for torque. The native `foc start`
diagnostic is a separate path and was not validated by these graph fixes.

## Evidence and remaining limits

Evidence is under `captures/direction_asymmetry/`:

- `stationary_field_alignment.json`
- `stationary_field_corrected_mapping.json`
- `verified_positive_35a.json` and `verified_negative_35a.json`
- `restart_before_neutral.json`
- `verified_atomic_startup_checks.json`
- `configuration_persistence.json`

The original 60 A, dyno-held regen-power comparison has **not** been rerun
with the fixes. Absolute torque, the independent ABB/encoder RPM discrepancy,
and absolute voltage-command scaling were not calibrated by these tests.
The 120° and 165° offset trials were rejected; the final configuration uses
one common 150° offset for both directions.
