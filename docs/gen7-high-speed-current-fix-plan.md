# Gen7 high-speed current regulation: source review and fix plan

Review date: 2026-09-22 (bench local date).
Status: investigation and proposed work only. No firmware/configuration changes,
builds, tests, live reads, motor commands, or flashes were performed for this review.

## Scope and evidence

Reviewed `Assets/Examples/foc_demo.json`, the main-image PWM/ADC/encoder/platform
code, bridge code generation, the existing spike recorder and MCP parser, and
the generated source in `build/torque-adc-fw-src`. That generated source reports
graph hash `dd8a7cd4d92a99c9`, matching the final runs in the saved session.

Relevant files:

- `/home/tliao/Desktop/forgptanalysis.jsonl`
- `captures/direction_asymmetry/bus100_full_positive_35a_extended.json`
- `captures/direction_asymmetry/bus100_full_negative_35a_extended.json`
- `captures/direction_asymmetry/bus100_current_review.png`

Final 100 V current-command windows, September 22 Pacific time:

| Command | Local time | Session `t`, seconds |
|---|---|---|
| +35 A | 22:50:18.799–22:50:28.608 | 8861.764–8871.573 |
| -35 A | 22:51:25.829–22:51:35.210 | 8928.794–8938.175 |

These export sections retain roughly one current sample per 1.28 seconds.
The separate host captures retain roughly 6.25 samples/second. Neither resolves
the current waveform. Values adjacent to current changes can include transition
behavior; host command time and telemetry reception time are not ADC timestamps.

The separate captures show id excursions before voltage clipping, then larger
excursions during clipping. The defects below are established from source;
their individual contributions to the observed ripple remain unmeasured.

## Confirmed defects

### 1. Voltage-to-duty conversion produces half the requested voltage

Locations:

- `Assets/Examples/foc_demo.json`: `Transforms.Svpwm` (line 2350).
- `Images/Gen7FW/MainProcessor/Src/Inverter/Drivers/PWM/pwm.cpp`:
  `PWM_SetDutyCycle` (line 132), `PWM_SetVoltageVector` (line 215).
- `Images/Gen7FW/MainProcessor/Src/Inverter/platform_api.cpp`:
  `platform_pwm_set` (line 40).

The graph divides phase voltages by Vdc, subtracts common mode, then uses
`duty_percent = 50 + 50 * normalized_voltage`.
The driver interprets duty as the high-side on-time percentage, without a
subsequent factor of two. Thus, in the ideal averaged inverter model,
`v_phase - v_neutral = 0.5 * requested_phase_voltage`.
Physical-voltage inputs instead require `50 + 100 * normalized_voltage`.

The native diagnostic's `PWM_SetVoltageVector` duplicates the same discrepancy.
The dimensionless modulation-index API `PWM_SetVoltageAngle` uses a different
contract: its factor 50 must not be changed mechanically.

The graph's command-vector cap of `2*Vdc/3` consequently delivers only `Vdc/3`
in ideal physical voltage magnitude. At 100 V that is about 33.3 V, with a
maximum duty span of about 57.7 percentage points. The observed top-end software
limit therefore does not establish full utilization of the inverter bus.

Fixing only the multiplier doubles effective proportional, integral, and
feedforward actuation. The existing gains cannot simply be left semantically
unchanged through that conversion. Physical flux/back-EMF units also need
confirmation before predicting a new achievable motor speed.

### 2. Anti-windup excludes the final vector saturation

Locations:

- Graph `Custom.PiCurrent` (line 3928), PiD/PiQ instances (lines 4852 onward).
- Generated TIM source: PiD line 646, PiQ line 703, Svpwm line 764.

Each PI independently clips to +/-`2*Vdc/3` and back-calculates only that axis's
clipping. Svpwm then rescales the combined vector. Its correction is never
returned to either integrator. A PI can therefore integrate against an applied
voltage that is smaller than its own supposedly limited output.

Example from the saved export at `t=8870.378906`: +2595.2 RPM, vd=-36.03 V,
vq=66.28 V in legacy command units, vector magnitude 75.44 V. Against an
approximately 66 V graph cap, downstream scaling is about 0.88. Current was
id=-17.57 A, iq=13.77 A during the +35 A command. Bus voltage comes from a nearby
slower telemetry group, so this ratio is approximate.

Use one d/q vector limiter and back-calculate each integrator from the voltage
actually selected by the complete limiting path. The standard principle is
described in [MathWorks' PMSM controller documentation](https://www.mathworks.com/help/sps/ref/pmsmcurrentcontroller.html).
This is a credible contributor during saturation, not an explanation for all
variation below saturation.

### 3. The critical-section API does not preserve interrupt state

Locations:

- `Images/Gen7FW/MainProcessor/Src/Inverter/platform_api.cpp:419`.
- `Lib/InverterCodegen/src/CodeGenerator.cpp:1113`.

`platform_critical_enter()` disables interrupts; `platform_critical_exit()`
unconditionally enables them. Generated bridge loads/stores call this pair.
An inner bridge access can therefore end an outer caller's interrupt exclusion.

Replace this with saved/restored PRIMASK semantics, with explicit support for
nested use. If the platform API signatures change, update all generated call
sites and supported platform implementations together. This is a correctness
defect; the saved logs do not establish that it caused the observed oscillation.

### 4. The spike recorder reports an incorrect time base for this setup

Locations:

- `.../Drivers/Sensors/PhaseCurrentADC.cpp:464`: recorder called from ADC ISR.
- `.../Drivers/Sensors/SpikeRecorder.cpp:80`: header hardcodes 5 kHz.
- `Source/RTECLI/src/Main.cpp:2090`: MCP assigns sample time as index/5000.

Actual current sampling was approximately 2500 Hz. A 64-sample buffer spans
25.6 ms at that rate, not 12.8 ms. Millisecond sample timestamps cannot resolve
400 microsecond spacing. Any analysis using the hardcoded rate has a factor-of-two
time/frequency error.

The existing recorder also captures the first raw current sample and a separately
read encoder angle before the generated Park transform; it does not capture the
exact averaged current and angle used by that transform. Commanded duty fields
are not necessarily the active, latched duties for the sampled interval.

## Confirmed architectural weaknesses; causal significance unproven

### 5. A control step can consume feedback from different ADC samples

ADC priority is 4; TIM1 priority is 5. The ADC publishes d/q through several
independent scalar bridges. Feedforward, PiD, PiQ, LogId and LogIq each load their
own bridges at different positions in the TIM step. Scalar critical sections
protect individual transfers, not the collection of values used by a step.
An ADC interrupt between those loads can mix sample generations.

Publish one frame containing id, iq, measurement angle, timestamp, sequence and
validity, then latch it once per control step. Feedforward, both axes and logged
feedback must use that same frame. Keep the atomic copy short. Do not use a
reader retry loop that can prevent a preempted lower-priority writer from finishing.

The encoder angle/timestamp read has a related coherence concern:
`EncoderADC::extrapolatedAngleDeg` reads angle and timestamp separately; the DMA
writer updates them separately. The higher-priority current ADC reader can
interrupt that publication. Include a coherent encoder snapshot in this work.

### 6. Current-sample and actuation timing are not explicitly tracked

`PWM_EnableFocMode` runs TIM control at twice the PWM frequency. The single rising
OC4REF trigger yields one current burst per PWM period: nominally 5 kHz control,
2.5 kHz feedback. Running a controller faster than its measurement stream is
not inherently wrong, but it does not provide fresh feedback every iteration.

`EncoderLeadComp` adds one TIM interval (200 microseconds here). That value is not
derived from the current sample timestamp, encoder acquisition timestamp, CCR
preload transfer, or the midpoint of the voltage application interval. At
2700 RPM with five pole pairs, 200 microseconds equals 16.2 electrical degrees.
Even smaller timing errors deserve measurement at this speed.

`m_last_burst_us` is recorded after JDR reads, not at the conversion midpoint;
the graph currently discards it. Dividing the 32-bit DWT counter before timestamp
subtraction also requires explicit wrap handling if used for elapsed time.

Instrument this timeline before choosing a new lead or scheduling model. One
candidate is PI integration once per fresh ADC sequence with the measured sample
interval, while inverse Park/modulation continues on TIM updates. Evaluate this
as a separate control change, not as an automatic fix for a proven rate bug.
Any domain/rate change must also update reference slew timing, initialization,
start/stop/reset behavior and execution-time budgets. In particular, the current
supervisor resets TIM graph state; moving integrators to ADC requires an explicit
lifecycle update.

### 7. Higher duty utilization needs a valid acquisition window

`PWM_FindSafeSamplePoint` checks a total quiet-window width and selects a point
near a triangle extremum. The scheduler falls back to CCR4=10 if no valid window
exists. It provides no validity flag to the current controller. The comment's
6 microsecond budget is not an explicit proof of both pre-trigger settling and
post-trigger conversion completion, and CCR4 scheduling must be checked against
the duty/preload interval that actually applies.

The present half-scale modulation leaves generous zero-vector windows. Restoring
full voltage utilization shrinks those windows. Before increasing the ceiling,
derive required margins from deadtime, sensor settling, ADC rank timing and timer
prescaler. Limit modulation to preserve a verified window, and propagate invalid
sample status instead of silently treating a fallback acquisition as valid.

## Proposed implementation sequence

### A. Make the measurements and data transfer trustworthy

1. Correct nesting/interrupt-state preservation.
2. Publish and consume coherent current and encoder frames.
3. Add a bounded, manually triggerable control capture with real sample timing:
   raw/averaged currents, exact Park angle, id/iq and references, unsaturated and
   limited d/q requests, PI integral contributions, bus voltage, active versus
   pending duties, sample validity, ADC sequence and timer phase.
4. Capture enough samples to observe slow regulation as well as electrical-cycle
   ripple; choose a fixed maximum after checking RAM and ISR cost. Export large
   captures to files with bounded/paginated access, not one oversized MCP reply.
5. Correct recorder rate metadata and the matching MCP parser together. Keep the
   legacy format recognizable; do not silently reinterpret old captures.

### B. Unify voltage units, limiting and anti-windup

1. Implement a shared physical-voltage modulator used by the generated graph and
   native diagnostic voltage-vector path. Preserve the normalized-index API.
2. First retain the old physical actuation using an explicit legacy-to-volts
   factor of 0.5 on the entire PI-plus-feedforward request and the old physical
   ceiling Vdc/3. Keep FRAM Kp=0.04/Ki=5 during this compatibility stage. This
   preserves the existing unsaturated effective gains and feedforward behavior.
3. Compute both axis requests together, apply one radial d/q limit, then update
   both integrators using the final limited result. Convert feedback into the
   same units as the PI state. Include any subsequent duty/window restriction;
   avoid an unreported second clipping stage. Preserve the anti-windup tracking
   time constant explicitly rather than accidentally changing it through a Kp
   unit conversion. Prefer an integrator state expressed as a voltage contribution.
4. Publish separate requested, limited and duty-derived voltage estimates.
   Duty-derived voltage is an ideal estimate, not measured terminal voltage.
   Document changed voltage telemetry semantics and the firmware version.
5. After this conservative stage is characterized, raise the available physical
   voltage toward a sampling-margin-limited linear ceiling, at most Vdc/sqrt(3).
   Do not use a circle of radius 2*Vdc/3 as if it were feasible at every angle:
   that radius reaches hexagon vertices and needs an explicit overmodulation
   strategy. Defer overmodulation for this repair.
6. Migrate legacy gains/feedforward units only as a separate, versioned operation.
   Halving stored Kp/Ki would preserve their unsaturated physical gain after
   removing the 0.5 adapter, but feedforward and anti-windup must be migrated too.
   Do not relabel or halve the physical motor flux measurement without calibration.

### C. Resolve remaining ripple using the corrected capture

1. Compare identical operating points before and after vector anti-windup.
2. If ripple remains before saturation, use acquisition/latch timestamps to
   evaluate deterministic PI scheduling and sample-to-actuation angle prediction.
3. Only after voltage and timing conventions are established, evaluate measured
   Ld/Lq decoupling separately. The present zero settings intentionally disable
   cross-coupling feedforward; that is not itself a coding defect. At 2700 RPM
   and 35 A, nominal |we*Lq*iq| is about 12.5 physical volts, so it can matter.
4. Keep encoder offset 150 degrees, polarity, pole count and the verified phase
   mapping through these stages. Changing those would confound this investigation.

## Validation to perform during implementation (not run in this review)

- Offline modulator checks: known vectors reconstruct the requested ideal
  line-to-neutral voltage; rotation and sign symmetry; 50/100 V buses; invalid
  inputs; bounded duties; linear-limit and sample-window constraints.
- Coupled PI checks: a combined request that exceeds the vector limit while
  each axis is individually inside its scalar bounds; prolonged saturation;
  release/reversal; feedforward saturation; bounded integral recovery.
- Timing checks: ADC preemption between old scalar read positions; coherent
  frame generations; nested PRIMASK preservation; timestamp wrap; duplicate or
  stale ADC frames; preload timing; start/stop/fault reset and neutral startup.
- Verify generated source, not just inline node snippets. Preserve the existing
  phase-mapping, fresh-JDR publication and atomic PWM-start fixes.
- Recorder/parser checks: actual rate, timestamps, versioning, partial captures,
  manual Studio route and MCP response-size bounds.
- Build current main firmware, rte and RTEStudio; run the relevant emitter tests
  and the required host compatibility suite:

  ```sh
  ctest --test-dir build --output-on-failure -R 'RTECLI_mcp_flash_integration|RteCli|RTEStudio_session_stale'
  ```

- When hardware work is resumed: matched directional measurements below clipping,
  followed by controlled approach to the verified voltage limit. Compare id mean,
  id/iq ripple using the same sample definition, tracking error, saturation fraction,
  recovery, acceleration and startup peak. No growing oscillation, coherent valid
  samples, bounded integrators, retained direction symmetry and no new startup
  spike are required; absence of a fault alone is insufficient. Quantitative
  ripple acceptance limits must follow sensor-noise and PWM-ripple characterization,
  not arbitrary bounds inferred from sparse telemetry.

## Host compatibility, persistence and deployment

Main MCU rebuild/reflash is required for these firmware changes. No coprocessor
protocol or reflash is anticipated. A changed spike capture format/rate requires
a coordinated rte parser update plus `MCP_AGENT_SETUP.md`,
`docs/automation-backend.md` and integration coverage. New registered commands
remain discoverable through help; new telemetry signals through the manifest.
Keep `control start` as graph control and `foc start` as the native diagnostic.

Inspect `rte_device_commands`, `rte_signal_info`, `rte_build_info` and the affected
capture tool against the eventual flashed image. These are future checks, not
authorization to contact the motor during this source-only task.

Persist no new FRAM values until the selected stage is characterized. Record the
old and new firmware identities and configuration versions, retain a rollback
image, and verify saved settings after reboot when that hardware work is resumed.

## Conclusion

Voltage scaling and incomplete vector anti-windup are the strongest concrete
control defects associated with the top-speed operating point. Coherent feedback,
interrupt-state preservation and correct capture timing are additional source
issues that should be addressed before interpreting another waveform. The
pre-saturation ripple's physical origin remains unresolved; source inspection
alone cannot distinguish current ripple, measurement aliasing and loop oscillation.
