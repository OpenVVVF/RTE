# Gen6 Hybrid ADRC+MPCC bring-up

Hardware current-loop demo: **ESO (ADRC) + mild deadbeat voltage → SVPWM**.

Same sensing / enable / PWM path as `mpcc_demo` (Mode 3). Compare against:

| Graph | Controller |
|-------|------------|
| `foc_demo.json` | PI current (FOC) |
| `ladrc_demo.json` | LADRC current |
| `hybrid_adrc_mpcc_demo.json` | Hybrid ADRC+MPCC |

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

## Critical: motor parameters

Use **calibrated** Gen6 values (same as FOC/ADRC/MPCC):

- `cal Motor.Resistance` → `CfgRs`
- `cal Motor.PMSM.Inductance` → `CfgLd` / `CfgLq` (sets ESO `b0 = 1/L`)
- `cal Motor.PMSM.FluxLinkage` → stored (unused by hybrid law, kept for KV parity)

Do **not** leave placeholder Ld/Lq if you have FRAM cal values.

## Safe first spin (no load motor)

1. Vdc ≈ **50–100 V** (same as known-good FOC). For ~1000 rpm no-load you likely need **≥80 V** if ψ ≈ 0.07 Wb (bemf ≈ ωe·ψ; at 50 V linear Vmax ≈ 27 V).
2. Encoder Sign/Offset match working `foc_demo`.
3. Confirm FRAM cal: `Ld/Lq/Rs/FluxLinkage` (graph defaults are placeholders; telem `mpcc_ld_h=0` means Config still reading 0 — controller falls back).
4. `IdVar = 0`, `IqVar = 0`, enable off → duties ~50%.
5. Enable with iq = 0; watch quiet bias.
6. Ramp `IqVar` slowly: **1 → 2 → 3 → 5 A**. Expect duties to leave ~50% and shaft to turn. If current spikes at a steady spin rate, check `OmegaDeriv.Unwrap=1` (2π wrap glitch).
7. Watch `cg_id` / `cg_iq`, duties, `hz_tim_isr`, overcurrent.

## Known failure: HybridADRCMPC-4 wrap spikes

`Transforms.Derivative` without unwrap sees Θe wrap as Δ ≈ −2π → ω spike → deadbeat/bemf commands huge V → 300–500 A. Fix: `Unwrap=1` on `OmegaDeriv` + hybrid recomputes ω with unwrap. Overcurrent now **zeros V** and clears integrators (no 0.15 floor).

## Tuning

| Param | Default | Notes |
|-------|---------|--------|
| `OmegaO` | 1500 | Mild residual ESO bandwidth |
| `DbScale` | 0.70 | Mode-3 deadbeat scale (needed for Gen6 low-L) |
| `I_Max` | 25 | Hard current clamp (zero V + clear Ki/ESO) |
| `Ts` | 0.0002 | Match `tim_isr` 5 kHz |
| `OmegaDeriv.Unwrap` | 1 | Required for Θe → ωe |

## Comparison procedure

1. Flash `foc_demo` → iq step → save telemetry.
2. Flash `ladrc_demo` → same iq step → save.
3. Flash `hybrid_adrc_mpcc_demo` → same iq step → save.
4. Compare rise time, overshoot, ripple, id coupling.

## If it trips

- Re-flash `foc_demo.json`.
- Check Ld/Lq/Rs vs cal.
- Lower `DbScale` (e.g. 0.25) or `IqVar`.
- Confirm Vdc ≥ 40 V (controller holds below that).
