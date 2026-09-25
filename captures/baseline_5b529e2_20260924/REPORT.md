# Baseline reflash and iq ripple check — September 24, 2026

Follow-up: [the matched 75 V investigation](INVESTIGATION.md) reproduced the
earlier improvement, with 1.80 A feedback iq AC RMS at approximately 2,078 RPM.
The initial lower-bus results below remain the record of the first tests.

The requested `5b529e2` main image was flashed successfully. High-speed iq
ripple remains: at a 35 A request and approximately 2,092 RPM, measured
controller feedback spans 18.28 A peak-to-peak, while raw iq spans 30.61 A.
The tests do not establish an isolated before/after firmware improvement:
there was no powered preflash capture, and operating conditions differ from
the earlier 75 V bench record.

## Image and preservation

- Original checkout: `061548c`, branch `coproc-flash-fixes`, initially clean.
  There were no changes to stash. Existing stashes and checkout were preserved.
- Used the clean detached `/tmp/rte-baseline` worktree at
  `5b529e2487bf0b285c6b5245a96783852c6cf3a7`, with `Assets/Examples/foc_demo.json`
  and `Images/Gen7FW/MainProcessor` as build inputs, Release configuration.
- Flashed ELF: `/home/tliao/.cache/rte/projects/e3c9c9f9ff02772d/artifacts/STM32CubeMX.elf`.
- ELF SHA256: `2031933c17d09ad6f86f1cb8d9064fd550739b73ac4949b7f2a5cb72a5b5f61b`.
- BIN SHA256: `3da04bb62a891ff989020a0072c592c5166739e1b44697319b37597ff83a2aa6`.
- Both hashes exactly match the previously documented successful bench image.
- Graph hash: `855dbcee90d611cb`; main MCU only, no coprocessor flash.
- The live graph already reported this newer graph hash before flashing;
  the earlier stale-generated-image hypothesis was not confirmed on hardware.
  Graph identity alone cannot identify changes to base firmware.
- Built current `rte` and `RTEStudio`; all four required CLI/flash/session tests
  passed. After flashing, checked firmware identity, discovered commands,
  iq signal availability, control status, and the bounded `ctrlcap` command.

## Settings and procedure

Bus approximately 49 V. d/q Kp=0.04 and Ki=5; encoder offset=150 degrees,
sign=+1, 10 poles, Motor.PhaseSwap=4, Ld=Lq=0; Iq slew=50 A/s.
Settings were read from the live firmware and are recorded in `session.json`.

The stationary capture used the prior persisted `Ctrl.VoltLimitPu=0.45`.
At the user's request, software phase-current protection was set to 650 A
using `ocset 650`. This is a runtime setting and resets to the baseline's
500 A default on reboot. The separate ADC analog watchdog is disabled in
this baseline by default; it was not changed. No test approached 650 A,
and these measurements do not validate high-current operation.

For the spin tests, `Ctrl.VoltLimitPu=0.54` was set, saved to FRAM, and
read back. At 2.5 kHz switching the firmware cap is
`(1 - 4 * 6 us * 2500) / sqrt(3) = 0.54271` times bus voltage.
0.54 leaves a small additional margin below that cap and stays below the
linear SVPWM ceiling. This is the code's sampling-margin limit, not an
independently measured maximum safe operating envelope.

Used main graph `control start`, never native diagnostic `foc start`.
10 A and 35 A runs began from rest with Id=0. Host monitoring stopped on
stale telemetry, invalid samples, loss of output enable, reported faults,
unexpected currents, or bounded speed/time. Captured at the end of each
accelerating run, then disabled control and zeroed IqVar. The dashboard RPM
lags the encoder angle: the 35 A arm decision was made near 1,612 dashboard
RPM, but the frozen waveform corresponds to approximately 2,092 RPM.
Each frozen capture was exported in bounded 16-frame pages while stopped.
Some responses arrived in multiple console chunks; all 512 ccA/ccB pairs
were collected and checked before analysis. No capture was rearmed to
recover missing export lines.

## Results

| Iq request | Fitted RPM | Mean iq | Feedback iq AC RMS | Feedback iq peak-to-peak | Raw iq peak-to-peak |
|---|---:|---:|---:|---:|---:|
| 0 A | 0 | 0.052 A | 0.314 A | 2.098 A | 4.010 A |
| 10 A | 558 | 10.000 A | 1.216 A | 6.570 A | 7.318 A |
| 35 A | 2,092 | 34.712 A | 2.975 A | 18.282 A | 30.606 A |

Each result uses 512 samples over approximately 102 ms at 5 kHz. AC RMS
means standard deviation about the capture's mean, not total RMS including
the requested torque current. Speeds are fitted from unwrapped electrical
angle with five pole pairs. These are accelerating windows, not steady-state
dyno points or external current-probe measurements.

Every capture had sample-valid=1 throughout. No voltage clipping occurred
in the zero/10 A captures. The 35 A capture clipped 26.2% of samples, with
minimum voltage scale 0.98077, bus 47.948–50.588 V, and sample age
23.39–26.74 us. During the preceding acceleration, lower-rate telemetry also
recorded bus sag to approximately 42 V and stronger limiting. Thus bus
headroom/supply behavior remains relevant; this run does not isolate its
contribution from PWM-dependent sampling ripple.

The earlier 75 V record at +35 A / +2,161 RPM reported 2.27 A feedback iq
AC RMS and 4.04 A raw iq AC RMS, versus 2.97 A and 6.28 A here. Different bus
voltage, utilization and clipping prevent treating this as a firmware-only A/B.
The old commit therefore does not eliminate the high-speed symptom, although
the captured controller-feedback range was just below 20 A peak-to-peak.
No filtering or control-code tuning was performed.

## Final state and artifacts

Main MCU remains on `5b529e2`; controller IDLE, outputs disabled, PWM MOE=0,
IqVar=IdVar=0, fault flags=0, gate ready, measured speed approximately 0 RPM.
Saved voltage limit remains 0.54; runtime software OC remains 650 A until reset.
The exact baseline also restores its old, effectively disabled MAX22530 OV/UV
defaults; it does not contain the later 190 V / 10 V threshold change.

Raw captures: `zero_ctrlcap.txt`, `ten_amp_ctrlcap.txt`,
`thirtyfive_amp_ctrlcap.txt`. `session.json` records command/monitor evidence;
`summary.json` holds calculated statistics; `iq_comparison.png` plots raw
and feedback iq. Reproduce the analysis with `python3 analyze.py`.
