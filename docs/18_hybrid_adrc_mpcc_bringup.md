# Gen6 Hybrid ADRC+MPCC bring-up (55 V demo)

Hardware current + **speed** loop: ESO (ADRC) + deadbeat voltage → SVPWM.

Same sensing / enable / PWM path as FOC. Compare against `foc_demo` / `ladrc_demo`.

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

## What changed for good 55 V results

| Issue (sessions 5–7) | Fix |
|----------------------|-----|
| Speed “not working” | `SpdMode=1`: **IqVar = mechanical rpm** (default 400) |
| Sync-loss blowups near ceiling | Ease deadbeat + shrink Iq as bemf→Vmax; Iq-priority voltage limit |
| Wrong flux placeholder 0.01 | Force / default **ψ = 0.07** Wb |
| Wrap spikes (session 4) | `OmegaDeriv.Unwrap=1` + local unwrapped ω |
| OC floor kept driving V | Hard OC: zero V + clear integrators |

At **55 V**, linear Vmax ≈ 30 V. With ψ≈0.07, no-load ceiling is roughly **700–750 rpm**. Target **400–500 rpm** for clean paper figures; do not demand 1000 rpm on 55 V without a higher bus.

## Critical: motor parameters

Use calibrated Gen6 FRAM values (same as FOC):

- `Motor.Resistance` → `CfgRs`
- `Motor.PMSM.Inductance.Ld/Lq` → `CfgLd` / `CfgLq`
- `Motor.PMSM.FluxLinkage.Wb` → **~0.07** (placeholders &lt;0.02 are overridden in firmware)
- Encoder Sign/Offset match working `foc_demo`

## Paper / demo spin (recommended)

1. Bus **55 V** (40 V minimum hold-off still applies).
2. `SpdMode = 1` (graph default).
3. `IdVar = 0`, enable off → duties ~50%.
4. Enable, then set **`IqVar = 400`** (rpm). Slew ≈ 120 rpm/s.
5. Expect: shaft accelerates to ~400 rpm, **phase |I| typically &lt; 12 A**, `hz_tim_isr` ~10 kHz, cost small.
6. Optional step: 400 → 500 rpm. Stay ≤ **600** at 55 V.
7. Log `mpcc_iq_a`, `mpcc_id_a`, `cg_iu/iv/iw`, `mpcc_omega_e` (rpm = ωe/5·60/2π), duties.

### Current mode (optional)

Set `SpdMode = 0`. Then `IqVar` is amperes again (try 3–6 A).

## Tuning

| Param | Demo default | Notes |
|-------|--------------|-------|
| `SpdMode` | 1 | rpm via IqVar |
| `SpdKp` / `SpdKi` | 0.03 / 0.08 | A per rpm / A per rpm·s |
| `DbScale` | 0.55 | Eases further near voltage ceiling |
| `I_Max` | 15 | Hard clamp |
| `PsiF` | 0.07 | BEMF feedforward |
| `PolePairs` | 5 | Gen6 |
| `OmegaDeriv.Unwrap` | 1 | Required |
| `SlewIq.Rate` | 120 | rpm/s in speed mode |

## If it trips

- Re-flash `foc_demo.json` to confirm hardware.
- Lower rpm setpoint (300–400).
- Confirm Sign matches FOC; ψ/L from cal.
- Check Vdc ≥ 50 V under load (sag → loss of headroom).
