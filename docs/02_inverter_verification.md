# Inverter Verification

## RTE hardware inverter (existing)

RTE does **not** expose eight discrete switching states. The hardware/graph path is:

- `math.svpwm` computes duty cycles from αβ voltage references
- Linear modulation limit: `Vdc/√3 × 0.95` (`math.svpwm/inline.cpp`)
- `hw.pwm.set_duty` writes duties to `platform_pwm_set()`

This is **mathematically different** from FCS-MPCC's discrete voltage vectors (magnitude `2/3·Vdc`), but **not an error** — it is a different modulation paradigm (averaged SVPWM vs. single vector per period).

## New simulation inverter (`TwoLevelInverter.h`)

Uses the user-specified stationary-frame equations:

\[
v_\alpha = \frac{2V_{dc}}{3}\left(S_a - \frac{S_b+S_c}{2}\right),\quad
v_\beta = \frac{V_{dc}}{\sqrt{3}}(S_b - S_c)
\]

### Verification results (all pass)

| Test | Result |
|------|--------|
| States 000 and 111 → zero αβ voltage | PASS |
| Six active vectors equal magnitude `2/3·Vdc` | PASS |
| Adjacent active vectors 60° apart | PASS |
| Opposite vectors anti-parallel | PASS |
| Line-to-line consistency | PASS |
| All 8 states valid | PASS |
| Magnitude vs RTE SVPWM linear limit documented | PASS |

Run: `./build/Lib/Simulation/Simulation_inverter_tests`

## Convention note

RTE SVPWM max **line-neutral** fundamental ≈ `Vdc/√3`. FCS active vectors have magnitude `2Vdc/3 > Vdc/√3`. MPCC therefore operates in a different voltage space than the existing SVPWM node — expected for finite-set control.
