# Why iq looked worse: matched 75 V repeat

The previous improvement is reproducible. With the same flashed `5b529e2`
image and gains, returning the bus from 49 V to 75 V reduced iq ripple at
almost identical current and speed. There is no evidence in these captures
that the dual-midpoint repair stopped working.

| Capture | Bus mean | Fitted RPM | Mean iq | Feedback iq AC RMS | Feedback iq peak-to-peak | Raw iq AC RMS | Raw iq peak-to-peak |
|---|---:|---:|---:|---:|---:|---:|---:|
| Earlier successful record | 74.20 V | 2,161 | 35.49 A | 2.27 A | 12.20 A | 4.04 A | 20.46 A |
| Today's lower-bus test | 49.25 V | 2,092 | 34.71 A | 2.97 A | 18.28 A | 6.28 A | 30.61 A |
| Today's 75 V repeat | 74.44 V | 2,078 | 35.34 A | 1.80 A | 11.30 A | 3.39 A | 17.21 A |

Each row is a 512-frame capture at approximately 5 kHz. The two runs today
are within 0.7% in fitted speed; they are accelerating windows, not
steady-state dyno measurements. The earlier success still had measurable
ripple, including 20.46 A raw iq peak-to-peak. The earlier report explicitly
said the repair reduced the controller's PWM-dependent feedback component
without proving that instantaneous winding-current ripple disappeared.

## What changed at the lower bus

The motor needed approximately 26 V of d/q voltage magnitude in both tests.
Producing that voltage from a 49 V bus requires much more PWM duty span:

| Quantity | Earlier 75 V | Today's 49 V | Today's 75 V |
|---|---:|---:|---:|
| Maximum duty span | 63.87% | 93.53% | 60.95% |
| Minimum estimated midpoint-to-edge time | 36.13 us | 6.47 us | 39.05 us |
| Samples with voltage limiting | 0% | 26.2% | 0% |

Edge times are calculated from software-tracked active duties and PWM
period, not measured gate waveforms. The lower bus leaves less time between
switching transitions and the ADC sample and puts the voltage controller
near its ceiling. This strongly implicates bus utilization and its associated
PWM-dependent measurement/current ripple in the worse result.

The repeated 75 V run temporarily used the earlier voltage fraction 0.45;
the 49 V run used 0.54. At 75 V the maximum selected voltage was 26.20 V,
well below even the minimum 0.45 cap of 33.26 V, so that cap was never active.
The improved 75 V result therefore did not come from clipping the controller
more aggressively. After the test, the live 0.54 value was restored; its
previously saved FRAM value was unchanged throughout.

The programmed 0.54 limit respects the firmware's linear-modulation and
nominal 6 us sampling guard. Those calculations do not independently
establish analog settling or low ripple at maximum utilization. Likewise,
`sample_valid=1` indicates the software timing/window checks passed; it
does not measure analog sample quality.

These captures cannot distinguish physical switching-current ripple from
sensor settling/pickup. An external synchronized current measurement is
needed to separate those mechanisms. No arbitrary extra filter or gain
change was used to make the graph look quieter.

## Verified: the previous estimator is functioning

Reconstructed every available feedback value from the raw values using
`0.25*x[n] + 0.5*x[n-1] + 0.25*x[n-2]`. Maximum discrepancy is 0.00075 A
in all three captures, consistent with the recorder's 0.001 A text precision.
All sample sequences advance by one and every sample-valid flag is one.
This rules out missed samples or silent estimator fallback in these windows.

The main raw component alternates between PWM extrema and varies with the
third electrical harmonic. Fitting that component gives raw-iq amplitudes
of 3.97 A (earlier 75 V), 7.38 A (49 V), and 3.54 A (75 V repeat).
It accounts for 69% of raw-iq variance in the 49 V capture. The corresponding
raw-id component accounts for 91% of raw-id variance there.

The three-tap estimator suppresses alternating midpoint error, but does
not perfectly reject a component whose amplitude changes with rotor angle.
At the 49 V capture's electrical frequency of 174.3 Hz, its calculated
gain at the PWM-minus-third-electrical sideband is 0.104; gain at the sixth
electrical harmonic is 0.627. The observed attenuation agrees with this
limitation. Residual electrical harmonics and slower variation remain.

## Separate source weakness: speed filtering feeds the control loop

`EncoderADC::diagnose()` updates RPM from a >=20 ms window using an EMA
coefficient of 0.02. Even at the fastest update interval, this is approximately
a one-second smoothing time constant. The graph uses that same smoothed RPM
for back-EMF feedforward and actuation-angle lead; `extrapolatedAngleDeg()`
also uses it.

Reconstructing feedforward speed from captured q-voltage request, q integral,
iq error and the verified Lambda=0.04/Kp=0.04 settings gives:

- Earlier 75 V: approximately 1,820 RPM used versus 2,161 RPM fitted.
- Today's 49 V: approximately 1,716 RPM used versus 2,092 RPM fitted.
- Today's 75 V: approximately 1,745 RPM used versus 2,078 RPM fitted.

This is a real control-input lag during acceleration and explains why the
dashboard stop/capture speed trails the waveform speed. It existed in the
earlier successful run too, so it does not by itself explain the new difference.
A separate control-rate speed estimate, with the slow display estimate kept
for presentation, deserves a focused change and matched validation. Merely
raising the existing filter coefficient could amplify encoder noise, so no
speculative change was flashed during this diagnosis.

Relevant source locations:

- `Images/Gen7FW/MainProcessor/Src/Inverter/platform_api.cpp:568`: estimator.
- `Images/Gen7FW/MainProcessor/Src/Inverter/platform_api.cpp:605`: voltage cap.
- `Images/Gen7FW/MainProcessor/Inc/Inverter/Drivers/Sensors/EncoderADC.h:442`:
  speed filter constants.
- `Images/Gen7FW/MainProcessor/Src/Inverter/Drivers/Sensors/EncoderADC.cpp:638`:
  speed update; line 275: angle extrapolation.
- `build/current-loop-fw-src/generated/domain_tim_isr_generated.cpp:565`:
  actuation lead; line 620: feedforward.

## Final state and reproduction

No firmware or gain changes during this investigation. Motor stopped near
0 RPM, graph IDLE, PWM MOE=0, outputs disabled, no faults. Main image remains
`5b529e2`. Bus approximately 74.3 V; live/saved voltage limit 0.54; software
OC remains at the previously requested runtime 650 A. No reboot occurred.

`bus75_repeat_ctrlcap.txt` contains the new high-rate capture.
`bus75_session.json` records its command and monitoring history plus final
state. `investigate.py` reproduces the estimator checks, fitted components,
speed reconstruction and timing calculations in `investigation.json`.
