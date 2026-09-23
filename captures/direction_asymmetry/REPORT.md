# Direction-dependent torque and power: bench measurements

> This is the **pre-fix baseline report**. Its unresolved-cause and
> no-firmware-change statements describe that earlier stage. See
> [the completed fix and validation](../../docs/gen7-direction-asymmetry-fix.md)
> for the identified 60° current-mapping error, startup fixes, final matched
> runs, and persistent configuration.

Date: 2026-09-22. Running graph `foc_demo`, reported hash `f5194253acc36e4f`.

The powered acceleration asymmetry is reproducible at matched ±15 A and ±25 A requests. The PWM-off coast-down comparison does not show enough passive-load asymmetry to explain it. The earlier claimed direction-specific 150°/120° offset optimum was not reproduced in the matched low-current regen sweep: both directions returned more power at 120° than at 150°. The exact electrical cause remains unresolved.

## Conditions and final state

- Gen7 inverter and Zero 10-pole motor; ABB supply separate from Sorensen.
- Bus approximately 49.3 V. d-current request 0 A.
- Both loops Kp 0.04, Ki 5 throughout. Ld/Lq cross-coupling feedforward disabled; lambda feedforward 0.04; knee compensation 0.
- Offset changes were live only, with 150° restored. No firmware changes, FRAM writes, gain changes, or encoder recalibration.
- Graph control used `control start`/`control stop`; native FOC diagnostic was not used.
- Tests ended with IqVar 0, IdVar 0, control IDLE, PWM off, and no reported fault. Dyno was stopped for the zero-current baseline and then left free-spinning for the acceleration tests; the final negative run coasted to rest.

Signs below refer to encoder direction. ABB forward is encoder-negative. Positive table power means energy returned to the Sorensen. Power readings were supplied by the operator from the Sorensen display, not inferred from `dclink_p_w`.

## 1. Matched dyno-held regen

The first 150° A/B pair used nominal ±500 RPM and opposing ±15 A current requests. Means below use the common time interval across the exported signals, approximately 14.7 seconds for the positive point and 9.9 seconds for the negative point.

| Quantity | Encoder-positive rotation | Encoder-negative rotation |
|---|---:|---:|
| Actual encoder RPM | +499.95 | −498.75 |
| iq mean, A | −15.14 | +14.78 |
| id mean, A | +0.240 | −0.019 |
| cg_vd_v, command V | −0.163 | −0.324 |
| cg_vq_v, command V | +11.018 | −10.138 |
| Sorensen regen, approximate W | 30 | 78 |

The command magnitudes were matched; the sampled iq means differ by about 2.4%. Those means come from aliased 128 Hz telemetry, not independent phase-current measurements. The positive 150° power point repeated at approximately 30 W at +505.6 RPM.

Both directions regenerated at 15 A. The direction returning more power is opposite to the supplied historical 60 A observation. Therefore the net-power result is not a simple proportional extrapolation of the reported 60 A asymmetry. The 60 A result was not rerun here, so its reproducibility and the transition current remain unknown.

### Voltage-model interpretation

The aligned-frame motor equation is

`vd = R*id + Ld*d(id)/dt - we*Lq*iq`.

At steady zero id, ±500 RPM and opposing 15 A give **+0.994 V in either direction**, using five pole pairs and Lq = 253 µH. This is a motor-terminal model, not automatically the controller readout. See the [PMSM model equations](https://www.mathworks.com/help/sps/ref/pmsm.html).

Inspection of `Assets/Examples/foc_demo.json` shows `cg_vd_v` and `cg_vq_v` connected to the PI outputs, including feedforward, before PWM conversion. The graph's duty conversion uses `50 + 50*(phase_voltage / Vdc - common_mode)`. Its ideal differential output is therefore half the requested voltage with ordinary 0–100% high-side duty semantics. The PWM driver documents those semantics.

Live recorder duties support the factor of two: reconstructed ideal q voltage was approximately **+5.43 V / −5.06 V**, versus commands **+11.02 V / −10.14 V**. This reconstruction uses 49.3 V bus, the captured mechanical angle ×5 +150°, and omits angle lead and semiconductor voltage drops; it is not a terminal-voltage probe measurement.

Using half the command as an approximate ideal inverter voltage, the 150° d-voltage model residual is about **−1.09 V / −1.14 V**. It is predominantly common between these two regen points. It cannot uniquely identify an encoder angle error: terminal-voltage errors, feedback errors, parameter accuracy, sampling, and frame timing remain in the residual.

The inspected graph also clamps the requested vector to 2*Vdc/3. Combined with its half-scale duty conversion, that would give an ideal physical ceiling of Vdc/3, approximately **16.4 V at 49.3 V**, if that clamp matches the running image. Thus the theoretical bus capability of Vdc/sqrt(3) ≈28.5 V cannot simply be assumed available through this path. This is a source-derived limitation, not a live saturation measurement; it does not establish the cause of the historical 800–900 RPM wall. The local generated build hash differs from the running hash, so source identity is not fully verified. No firmware was changed on this basis.

## 2. Matched offset sweep at 15 A

Negative rotation was approximately −498.5 to −498.9 RPM. The positive sweep was approximately +505.4 to +506.0 RPM. The speed mismatch is about 1.4%, and is recorded rather than silently normalized away.

| Offset, electrical degrees | Positive-speed regen, W | Negative-speed regen, W | Positive-speed vd command, V | Negative-speed vd command, V |
|---:|---:|---:|---:|---:|
| 120 | ~90 | ~110 | −5.648 | +5.465 |
| 135 | ~65 | ~100 | −3.092 | +2.667 |
| 150 | ~30 | ~78 | −0.190 | −0.324 |
| 165 | ~50 | ~47 | +2.842 | −3.360 |

The positive 150° row is the repeated baseline. At 120°, the operator described the negative-direction reading as “110 ish,” with little obvious difference from 135°. These are approximate observations without calibrated uncertainty bars.

Both directions improved from 150° toward 120° in the tested interval. This does not reproduce a positive-direction optimum at 150° paired with a negative-direction optimum at 120°. It also does not establish a global optimum: no offsets below 120° were tested. The small 120°/135° difference in negative rotation is not sufficient to select a precise optimum. These results do not specifically implicate encoder nonlinearity, so encinl and ellipse calibration were not repeated.

Plot: [offset_sweep.png](offset_sweep.png). Numeric means: [numeric_summary.csv](numeric_summary.csv).

## 3. Free-spin acceleration and coast-down

The operator placed the ABB in its free-spinning condition. Each powered run started near rest at 150° offset and zero d-current request. Runs were stopped at 600 indicated RPM or 15 seconds; one positive 15 A repeat stopped at 400 RPM to measure coast-down. Current slew remained 50 A/s.

| Measurement | Encoder-positive | Encoder-negative |
|---|---:|---:|
| 15 A speed at 3 s, absolute RPM | 181 | 195 |
| 15 A speed at 6 s, absolute RPM | 321 | 403 |
| 15 A speed at 9 s, absolute RPM | 380 | 550 |
| 15 A acceleration, 250–350 RPM, RPM/s | 32.5 | 69.1 |
| 25 A time to 600 RPM, s | 6.53 | 4.56 |
| 25 A acceleration, 400–500 RPM, RPM/s | 90.4 | 147.6 |
| 25 A acceleration, 500–600 RPM, RPM/s | 61.4 | 180.4 |
| Coast after 25 A, 400–500 RPM deceleration, RPM/s | 84.0 | 87.6 |

The 25 A speed-band times are interpolated from approximately 6–7 Hz host polling of the reported encoder speed. A linear fit within 400–500 RPM gives 91.7 versus 147.6 RPM/s, consistent with the crossing-time calculation. The negative 500–600 RPM interval contains only three interior polling observations; its approximately 3× acceleration ratio is less robust than the 400–500 RPM result.

In the 400–500 RPM interval, observed current means were +26.56 A and −24.29 A from eight and five aliased polling samples, respectively. These do not show the negative run simply receiving a larger reported current, but are insufficient for accurate true-current matching or torque-per-ampere calibration.

Coast-down is measured only after PWM-off telemetry. In the 400–500 RPM band its directional difference is only 3.6 RPM/s, versus a powered acceleration difference of 57.1 RPM/s. Passive drag therefore does not account for the observed powered asymmetry in that range. Under the assumption that coast drag represents the passive component while powered, adding it back gives 174.4 versus 235.2 RPM/s of effective drive acceleration, a ratio of **1.35**. This is torque per common inertia, not an absolute Nm measurement, and it does not separate current calibration, active motor losses, inverter voltage error, or true torque generation.

The 15 A coast comparison supports the same direction: deceleration through 250–350 RPM was 68.4 positive versus 77.4 negative. The weaker powered direction did not have the larger passive coast loss.

The 600 RPM cutoff is an MCP/host stop, not a firmware speed limiter. Highest logged speeds during stopping were approximately **622 RPM positive and 716 RPM negative** in the 25 A tests. Both remained below 800 RPM. The final negative run coasted to below 3 RPM. The positive 25 A coast log stopped around 294 RPM when Studio temporarily marked the speed signal `control_stopped`; fresh near-zero speed was subsequently observed before the negative run. No fault was reported. One initial 25 A attempt was stopped immediately because the output-enable telemetry still showed zero during startup; it was excluded and retained as `free_spin_positive_25a_aborted_start.json`.

Plot: [free_spin_comparison.png](free_spin_comparison.png). Calculations: [motion_summary.json](motion_summary.json).

## 4. Feedback and recording limitations measured directly

With the shaft stationary, PWM off, outputs disabled, and encoder speed averaging −0.052 RPM, a five-second window of 500 received samples gave:

| Signal | Mean, A | Sample standard deviation, A |
|---|---:|---:|
| cg_iu_a | +0.733 | 0.599 |
| cg_iv_a | −0.971 | 0.415 |
| cg_iw_a | +0.238 | 0.417 |

The feedback has a nonzero baseline even without driven winding current. This is a concrete current-path finding, not proof that it causes the torque asymmetry. The third phase is reconstructed; a zero three-phase sum cannot independently validate the transducers.

The recorder advertised 64 samples at 5 kHz, but sample-index/tick regressions gave approximately **2499 Hz and 2494 Hz**. Live active telemetry reported TIM ISR 5000 Hz and ADC ISR 2500 Hz; idle TIM and ADC rates were about 2500 Hz. The recorder runs from the ADC path. Only **52/64 and 57/64** lines were returned in the two captures, despite the tool's `complete` field. Automatic recorder time/frequency metrics based on 5 kHz are therefore unsuitable for these records. The saved samples retain index, firmware milliseconds, encoder angle, currents, and duties. Threshold was temporarily 8 A and restored to 50 A.

A recorder sampling once per 2.5 kHz PWM cycle cannot establish the full within-cycle PWM ripple. Neither it nor the 128 Hz telemetry constitutes an independent current measurement. Partial, synchronous captures do not rule out a current-feedback gain or sampling problem.

## RPM discrepancy

The operator confirmed that the earlier ABB 1084 RPM value was shaft-speed indication, rather than the command reference. RTE 1130 versus ABB 1084 is +46 RPM, or **+4.24% relative to ABB**. No simultaneous steady pair was captured in this session. A single pair cannot determine which instrument is wrong or distinguish scale error from display filtering during acceleration. No correction factor was applied. A mismatch of this size alone would not explain the measured powered/coast differences.

## What is established and what remains open

Established: a direction-dependent powered acceleration difference at low current and moderate speed; passive coast drag too similar to account for it; 15 A net regen in both directions; no reproduction of the historical direction-specific offset optimum in the tested range; nonzero stationary current offsets; a commanded-voltage scale issue; and an incorrect recorder-rate label.

Not established: phase-transducer gain accuracy, actual torque in Nm, true terminal dq voltages, the cause of the 60 A net-power reversal, or the cause of the 800–900 RPM trip wall. No shaft-compliance or encoder-nonlinearity theory was used to fill these gaps.

The next measurement that would separate the remaining electrical causes is simultaneous independent phase-current and terminal-voltage measurement in a matched dyno-held motoring A/B, ideally with ABB torque. That would determine whether equal reported iq corresponds to equal physical current and whether the commanded voltage corresponds to the applied voltage. Increasing speed using the same unverified feedback would characterize the symptom further but would not independently calibrate that path.

Reproduce the numerical summaries and plots with `python3 captures/direction_asymmetry/analyze.py` from the repository root. Raw exported histories, bounded polling logs, operator readings, and recorder captures are stored beside this report.
