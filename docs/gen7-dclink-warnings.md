# Gen7 DC-link warning diagnosis

Bench session: October 9, 2026. Source: user-reported 50 V / 50 A supply.

## Observed triggers

All three warning configuration keys were absent from FRAM, so firmware used
its defaults: OV warning above 55 V, UV derate below 40 V, and a 10 A UV
current ceiling. Warning entry has no software debounce. Live OV clears
below 53 V and live UV clears above 42 V, but their warning fault bits latch.
Both names can therefore remain visible at a healthy 49.27 V.

The exported Studio history shows repeated crossings during nearly constant
full-throttle requests. One interval (Studio time 2792.0–2793.5 s) contained:

| Measurement | Observed range |
|---|---:|
| DC-link voltage reading | 36.51–65.98 V |
| Graph Iq reference before protection | 49.91–50.00 A |
| Encoder mechanical speed | +774–+1268 RPM |

Many OV excursions contained several transmitted samples spanning about
10–22 ms; UV excursions were shorter. These were repeated events, not just
one startup sample. A later 30-second window stayed between 47.51 and
51.03 V. The ADC driver's diagnostic counters reported zero DMA errors and
zero failed starts. CRC was disabled, so a zero CRC-error counter does not
establish data integrity.

The local CSV is `build/audit-captures/dclink_warnings_20261009.csv`.
These are sensor readings; an independent bus-voltage measurement is needed
to distinguish real supply/capacitor excursions from measurement artifacts.

## Corrections

The graph's 50 A/s slew limiter introduced a one-second full-scale throttle
delay, including on release. It was already settled near 50 A during the
captured oscillations, so the delay alone does not explain those events.
The graph now transfers the bounded 0–50 A target directly to its enable
gate across the application/control domain bridge. The sensor still updates
every 16 ms.

The previous OV command clamp treated all positive Iq as regeneration. In
this run, positive Iq accelerated the rotor in the positive direction: that
is motoring. That clamp could cut forward current when a voltage excursion
crossed 55 V and release it below 53 V, contributing to hunting. OV now
blocks torque opposing the sign-corrected electrical rotor direction, with
a small standstill deadband, and leaves motoring available in either
direction. UV retains its symmetric current ceiling. Warning thresholds
and critical comparator protection are unchanged.

`cg_iq_ref_a` remains the graph reference before protection.
`cg_iq_effective_a`, `vdc_regen_disabled`, and `vdc_derate_limit_a` now expose
the applied reference and live clamps. This distinguishes an active clamp
from an old latched warning. The fixes require only the main MCU image.

## Verification and deployment

Graph validation and the firmware safety tests passed, including all four
motoring/braking direction cases, UV limits, standstill, and nonfinite inputs.
Current `rte` and `RTEStudio` were built and all four required MCP/CLI/session
integration tests passed. The main MCU was flashed through the Studio lease.
The deployed graph hash is `2e13b332c340a514` (82 nodes, 36 manifest signals).
ELF SHA-256:
`23008cc58a08f66acda1cb954f4ecf3a5a76eceb1b56392ba5eb8a635aab3ead`.

Live MCP confirmed command discovery and all three new signals. After the
reflash, voltage was 49.27 V, both live voltage clamps were inactive,
the effective Iq reference was zero, and control was IDLE with PWM disabled.
No powered motor sweep was run after this update; elimination of the
captured voltage oscillations has not been established.
