# Hardware FCS-MPCC bring-up (spin the motor)

Goal: spin Gen6 with predictive current control using the **same sensing /
enable / PWM path as working FOC**, replacing only the current regulator.

## What to flash

Use the RTE branch `feature/fcs-mpcc-three-phase-pmsm` and graph
`Assets/Examples/mpcc_demo.json`.

```bash
cd "/Users/saeed/Desktop/Journal Paper 2026/GitHub Repository/RTE"

cmake -B build -G Ninja && cmake --build build -j8

./Tools/build_flash_graph.sh \
  --graph Assets/Examples/mpcc_demo.json \
  --fw-build-dir build/mpcc-demo-fw \
  --fw-src-dir build/mpcc-demo-fw-src \
  --clean
```

## Critical settings (must match working FOC)

| Item | Value |
|------|--------|
| `Mode` | **3** (deadbeat voltage → SVPWM). Do **not** use 0–2 on Gen6. |
| Encoder Sign | Same as FOC (`Motor.Encoder.SinCos.Sign`, default **+1**) |
| Offset / Poles | Same KV keys as `foc_demo` |
| `Ld/Lq/Rs/PsiF` | Calibrated FRAM values (graph wires `CfgLd`…`CfgPsi`) |
| PWM path | `Mpcc.V_Alpha/V_Beta` → `Svpwm` → `PwmOut` |

Modes 0–2 enumerate full inverter vectors. On ~100 µH motors at Ts=200 µs
they predict huge Δi and stick on zero vectors. Mode 3 computes a continuous
voltage like FOC PI, then uses the same SVPWM block.

## Safe first-spin procedure

1. Confirm FOC still spins (`foc_demo`) so encoder polarity/offset are trusted.
2. Flash `mpcc_demo`. Connect telemetry / NodeGUI.
3. Enable **off**. Duties should sit near **50%**. Check `mpcc_enc_sign` matches FOC.
4. Set `IdVar = 0`, `IqVar = 0`, then enable.
5. Ramp `IqVar` slowly (slew is in the graph), e.g. 1 → 3 → 5 A.
6. Watch:
   - `mpcc_id_a`, `mpcc_iq_a` track refs (id≈0, iq→IqVar)
   - `mpcc_dv` / `mpcc_dw` move **away from 50%** when Iq≠0 (voltage authority)
   - `hz_tim_isr` ≈ 5–10 kHz
   - no sustained overcurrent
7. If currents explode or duties stay at 50%: re-flash `foc_demo`, verify Sign/offset, then retry.

## Higher speed (match FOC ~2000 rpm @ 100 V)

1. Confirm `cg_vdc_v` ≈ 100 and calibrated `PsiF` / `Ld` / `Lq` (same FRAM as FOC).
2. Keep **Mode = 3**. Firmware now allows ωe up to ±3000 rad/s (~5700 rpm @ 10 poles).
3. Raise `IqVar` toward the same current FOC uses at 2000 rpm (watch `cg_iq_a` on FOC).
4. Iq slew defaults to **50 A/s** (same as `foc_demo`).
5. Expect duties to leave mid-rail more as speed rises; if `id` drifts, back off Iq or re-check encoder Sign/offset.

## If something trips

- Re-flash `foc_demo.json` immediately.
- Confirm Sign matches the FOC run that worked (do not force the opposite polarity).
- Lower `IqVar` / raise caution on `I_Max`.
- Keep `Mode = 3`.
