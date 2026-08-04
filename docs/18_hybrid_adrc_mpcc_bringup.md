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

1. Vdc ≈ **50–100 V** (same as known-good FOC).
2. Encoder Sign/Offset match working `foc_demo`.
3. `IdVar = 0`, `IqVar = 0`, enable off → duties ~50%.
4. Enable with iq = 0; watch quiet bias.
5. Ramp `IqVar` slowly: **1 → 2 → 3 A**. Expect duties to leave ~50% and shaft to turn. If current is huge again, check encoder Sign vs FOC.
6. Watch `cg_id` / `cg_iq`, duties, `hz_tim_isr`, overcurrent.

## Tuning

| Param | Default | Notes |
|-------|---------|--------|
| `OmegaO` | 1500 | Mild residual ESO bandwidth |
| `DbScale` | 0.70 | Mode-3 deadbeat scale (needed for Gen6 low-L) |
| `I_Max` | 25 | Soft current scale-back (keeps some voltage) |
| `Ts` | 0.0002 | Match `tim_isr` 5 kHz |

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
