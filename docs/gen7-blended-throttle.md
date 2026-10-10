# Gen7 blended throttle

`Assets/Examples/foc_demo.json` uses the reusable `Control.BlendedThrottle`
node (`ThrottleIq`) in `app_loop`. The node editor exposes its pedal and
plausibility inputs, two speed bridges, six config nodes, and telemetry
outputs. Rebuild and reflash **the main MCU only** to install this graph.

## Current settings

| Config key | Default | Meaning |
| --- | ---: | --- |
| `Ctrl.Thr.MaxA` | 150 | Full-throttle forward Iq, A |
| `Ctrl.Thr.RegenMaxA` | 50 | Maximum regen Iq magnitude, A |
| `Ctrl.Thr.NeutralMax` | 0.30 | Neutral pedal fraction at high speed |
| `Ctrl.Thr.NeutralFullRpm` | 800 | Motor mechanical RPM where neutral reaches 30% |
| `Ctrl.Thr.RegenStartRpm` | 50 | Motor mechanical RPM below which neutral and regen are zero |
| `Ctrl.Thr.RegenFullRpm` | 300 | Motor mechanical RPM where the full regen magnitude is available |

These speeds are motor RPM, not wheel RPM or road speed. There is no
temporal slew limiter. The existing throttle sensor updates every 16 ms;
the application step maps the latest reading directly to the ISR reference.
The speed-dependent fade does not accumulate or delay a pedal command.
The graph uses the timestamped control speed estimate (2 ms window and
10 ms filter), avoiding the dashboard RPM estimate's approximately one-second
lag at stopping or direction changes. DC-link regen classification uses the
same electrical speed estimate.

`config set <key> <value>` changes a setting live. `config save <key>`
persists it to FRAM. The same commands are available in Studio's Runtime
console and through `rte_device_command_response`. New, unsaved keys use
the defaults above. `Ctrl.Thr.RegenMaxA = 0` disables regen; setting
`Ctrl.Thr.NeutralMax = 0` gives a linear 0–`Ctrl.Thr.MaxA` pedal map.

## Mapping

Let `s = abs(timestamped control mechanical RPM)` and `p = clamp(throttle A, 0, 1)`.

```
neutral = NeutralMax * clamp((s - RegenStartRpm) /
                            (NeutralFullRpm - RegenStartRpm), 0, 1)
regen_available = RegenAmps * clamp((s - RegenStartRpm) /
                                   (RegenFullRpm - RegenStartRpm), 0, 1)

p > neutral: Iq = MaxAmps * (p - neutral) / (1 - neutral)
p = neutral: Iq = 0
p < neutral: Iq = -sign(electrical RPM) * regen_available *
                 (neutral - p) / neutral
```

Regen is zero when neutral is zero or electrical RPM is zero. Electrical
RPM includes the encoder calibration sign, so regen opposes actual FOC
rotation even when raw mechanical encoder direction is inverted. Above
neutral, positive Iq keeps the existing forward drive direction. This is
a forward throttle; it does not select reverse drive when rolling backward.

For positive electrical rotation, the current settings give:

| Motor speed | Neutral pedal | Released Iq | Half-pedal Iq | Full-pedal Iq |
| ---: | ---: | ---: | ---: | ---: |
| 0–50 RPM | 0% | 0 A | +75 A | +150 A |
| 175 RPM | 5% | −25 A | +71.05 A | +150 A |
| 425 RPM | 15% | −50 A | +61.76 A | +150 A |
| ≥800 RPM | 30% | −50 A | +42.86 A | +150 A |

Invalid plausibility, nonfinite pedal/speed/settings, negative current
limits, neutral outside `[0,1)`, negative start RPM, or a full-speed point
at/below start RPM zero all node outputs. Pedal values outside `[0,1]` are
clamped. The neutral transition is continuous; there is no extra pedal
deadband. The existing calibrated A idle margin remains in the sensor.

## Monitoring and protection

- `thr_a_norm`: bounded, calibrated pedal fraction.
- `thr_neutral`: current neutral pedal fraction, 0–1.
- `thr_regen_available_a`: speed-limited regen magnitude before voltage protection.
- `cg_iq_target_a`: signed blended request before the enable gate.
- `cg_iq_ref_a`: gated ISR reference before voltage protection.
- `cg_iq_effective_a`: reference after DC-link protection.
- `cg_iq_a`: measured Iq feedback.

`Ctrl.ThrottleEnable` still gates Iq, and the normal control lifecycle and
DC-link protection still govern actuation. OV blocks regeneration above
55 V until voltage drops below 53 V; UV retains its current ceiling. A
50 V bench supply may not absorb returned energy, so regen testing can
activate the OV clamp. The 50 A regen setting is motor Iq, not a DC charging
current limit. This map does not add battery charge management.

Host tests compile the actual node body and cover standstill, moving
neutral, low-speed fade, both encoder directions, pedal bounds, invalid
inputs/settings, continuity, monotonic forward response, and immediate
updates. The graph test checks the embedded node against the reusable
template and checks its explicit speed and Iq bridges. Powered motor
behavior still needs bench verification after deployment.

## Initial deployment check — 2026-10-09

The main MCU was flashed through the Studio-owned bridge. Live
`rte_build_info` reports graph hash `eef7fa7616259d42`, 90 nodes, and 38
manifest signals. ELF SHA-256:
`24961ef1cf5ad1b2ebf5b1b0542484f964212ef4370ce2d606c0c095fd4aff9c`.
Both new signals are live with the expected units, all six `config get`
responses matched the initial 10 A / 20% / 1000 RPM defaults, and the help catalog is complete (53 commands).
Released pedal at rest reports zero neutral, regen availability, target,
gated reference, and effective reference. Control is IDLE with PWM off;
main and coprocessor report no faults. The 19 focused host/codegen tests
and the main safety test script passed. No powered motor run was performed.

## Stronger regen tuning — 2026-10-09

After the first test, regen was raised from 10 A to 20 A and neutral from
20% at 1000 RPM to 30% at 800 RPM. The 50–300 RPM regen fade and +50 A
full-throttle limit remain. Control was stopped before applying the three
live settings. Each value was saved individually and confirmed in the
FRAM configuration listing; the graph's config defaults and node parameter
fallbacks match. No reflash was needed, so the running graph identity above
still describes the original compiled defaults, overridden by these saved
settings. Validation and all 11 focused throttle/host tests passed. Released
pedal at rest still requests zero, and outputs were left disabled. Powered
behavior of this stronger setting has not been tested by the agent.

## Requested current limits — 2026-10-09

Maximum regen is now 50 A and full-throttle forward Iq is 150 A. Both
settings were applied with control stopped, saved to FRAM, and verified
through live reads and the stored configuration listing. Graph config
defaults and node parameter fallbacks match. Neutral remains 30% at
800 motor RPM with the existing 50–300 RPM regen fade. No reflash was
needed. Graph validation and all 11 focused throttle/host tests passed;
powered behavior at these current limits has not been tested by the agent.
