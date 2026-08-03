# Gen6 LADRC current-loop bring-up

Experimental **Linear Active Disturbance Rejection Control (LADRC)** current
regulators on the same Gen6 sensing / SVPWM path as FOC.

**Does not modify** `foc_demo`, `mpcc_demo`, `Control.Pi`, or `Control.Mpcc`.

## What was added

| Path | Role |
|------|------|
| `Assets/NodeTemplates/Control.Ladrc/` | 1st-order LADRC node (PI drop-in) |
| `Assets/Examples/ladrc_demo.json` | FOC graph with `PiD`/`PiQ` → `Control.Ladrc` |

Branch: `feature/gen6-ladrc-spin`

## Flash

```bash
cd "/Users/saeed/Desktop/Journal Paper 2026/GitHub Repository/RTE"

cmake -B build -G Ninja && cmake --build build -j8

./Tools/build_flash_graph.sh \
  --graph Assets/Examples/ladrc_demo.json \
  --fw-build-dir build/ladrc-demo-fw \
  --fw-src-dir build/ladrc-demo-fw-src \
  --clean
```

## Defaults (Gen6-ish)

| Axis | `B0` ≈ 1/L | `OmegaC` | `OmegaO` |
|------|------------|----------|----------|
| Id (`PiD`) | 14286 (70 µH) | 800 rad/s | 2400 rad/s |
| Iq (`PiQ`) | 8333 (120 µH) | 800 rad/s | 2400 rad/s |

`Dt = 200 µs`. Output clamp uses the same `Vdc/√3·0.95` rule as `Control.Pi`.

## Safe first spin

1. Prefer **Vdc ≈ 50–100 V** (same as a known-good FOC run).
2. Confirm encoder **Sign / Offset** match working `foc_demo`.
3. `IdVar = 0`, `IqVar = 0`, then enable.
4. Ramp `IqVar` slowly: **1 → 3 → 5 A**.
5. Watch `cg_id` / `cg_iq` (or log keys) track refs; duties leave ~50%.

## Tuning knobs

- Raise `OmegaC` for faster tracking (more hiss if too high).
- Keep `OmegaO ≈ 3–5 × OmegaC`.
- Set `B0 = 1/L` from FRAM `Ld`/`Lq` if defaults feel weak/harsh.

## If it misbehaves

- Re-flash `foc_demo.json` for a known baseline.
- Lower `OmegaC` / `IqVar`.
- Do not confuse this graph with `mpcc_demo` (Mode 3 MPCC).
