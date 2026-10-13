# Gen6 Hybrid ADRC+MPCC bring-up

Same **test approach as FOC**: set **`IqVar` in amperes**, enable, watch dq / phase currents and spin.

Hardware path matches `foc_demo`: sense → Park → controller → **Vαβ → SVPWM → PWM**.

| Graph | Inner current law |
|-------|-------------------|
| `foc_demo.json` | PI |
| `ladrc_demo.json` | LADRC |
| `hybrid_adrc_mpcc_demo.json` | Hybrid ADRC + deadbeat (Mode-3 style) |

## Flash

```bash
cd "/Users/saeed/Desktop/Journal Paper 2026/GitHub Repository/RTE"

cmake -B build -G Ninja && cmake --build build -j8

./Tools/build_flash_graph.sh \
  --graph Assets/Examples/hybrid_adrc_mpcc_demo.json \
  --fw-build-dir build/hybrid-adrc-mpcc-fw \
  --fw-src-dir build/hybrid-adrc-mpcc-fw-src \
  --clean
```

## FOC-like spin (default)

1. Bus **~55 V** (hold if Vdc &lt; 40 V).
2. **`SpdMode = 0`** (graph default) → **`IqVar` is amps**, same as FOC.
3. `IdVar = 0`, enable off → duties ~50%.
4. Enable; ramp **`IqVar`: 1 → 2 → 3 → 5 A** (slew ~50 A/s like FOC).
5. Expect clean phase current and shaft speed rising with Iq (no-load ~hundreds of rpm at 55 V).
6. For paper figures stay in a comfortable band; at 55 V / ψ≈0.07 the voltage ceiling is roughly **700 rpm** — do not expect 1000 rpm without a higher bus.

## What else is in the hybrid (not FOC-operator changes)

These stay for robustness from sessions 4–6; they do **not** change the Iq-in-amps workflow:

- Θe unwrap for ω
- ψ default / reject 0.01 placeholder → **0.07**
- Milder deadbeat near voltage ceiling, Iq-priority clamp, light field-weakening
- Hard overcurrent (zero V)

Optional: `SpdMode = 1` makes `IqVar` a rpm setpoint (not used for FOC-comparable tests).

## Tuning

| Param | Default | Notes |
|-------|---------|--------|
| `SpdMode` | **0** | FOC-like current mode |
| `DbScale` | 0.55 | Deadbeat scale |
| `I_Max` | 15 | Hard current clamp (A) |
| `PsiF` | 0.07 | BEMF feedforward |
| `SlewIq.Rate` | 50 | A/s (same idea as FOC) |
| `OmegaDeriv.Unwrap` | 1 | Required |

## If it trips

- Re-flash `foc_demo.json` and confirm the same Iq step still works.
- Match encoder Sign/Offset to FOC.
- Confirm Ld/Lq/Rs/ψ cal.
- Lower Iq or DbScale slightly.
