# Hardware FCS-MPCC bring-up (spin the motor)

Goal: spin Gen6 with predictive current control using the **same sensing /
enable / PWM path as working FOC**, replacing only the current regulator.

## What to flash

Use the RTE branch with `mpcc_demo` (e.g. `feature/gen6-foc-vs-mpc-simulation`
or `feature/fcs-mpcc-three-phase-pmsm`) and graph
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

## Critical settings

| Item | Value |
|------|--------|
| `Mode` | **3** (deadbeat voltage → SVPWM). Do **not** use 0–2 on Gen6. |
| `Vdc` | **≥ 40 V** required (controller holds if lower). Prefer **50 V** for quiet bring-up, **~100 V** for higher speed. |
| Encoder Sign | Same as FOC (`Motor.Encoder.SinCos.Sign`, default **+1**) |
| Offset / Poles | Same KV keys as `foc_demo` |
| `Ld/Lq/Rs/PsiF` | Calibrated FRAM values (graph wires `CfgLd`…`CfgPsi`) |
| `I_Max` | Graph default **15 A** (soft limit; only Iq ceiling) |
| Iq slew | **10 A/s** |
| PWM path | `Mpcc.V_Alpha/V_Beta` → `Svpwm` → `PwmOut` |

Mode 3 is softened for smooth spin: `db_scale≈0.45`, weak integral,
250 ms soft-start. `|Iq*|` is limited only by `I_Max` (no hidden Vdc Iq clamp).

Modes 0–2 enumerate full inverter vectors. On ~100 µH motors at Ts=200 µs
they predict huge Δi and stick on zero vectors.

## Safe first-spin procedure (smooth MPC @ 50 V)

1. Set DC bus to **~50 V**. Confirm telemetry `vdc_v` / `cg_vdc_v` ≈ 50 **before** enable.
2. Flash `mpcc_demo`. Connect telemetry / NodeGUI.
3. Enable **off**. Duties should sit near **50%**. Check `mpcc_enc_sign` matches FOC.
4. Set `IdVar = 0`, `IqVar = 0`, then `control start`.
5. Ramp `IqVar` slowly: **1 → 2 → 3 A**. Stay there if rotation is smooth.
6. Watch:
   - `mpcc_id_a`, `mpcc_iq_a` track refs (id≈0, iq→IqVar)
   - duties move **away from 50%** when Iq≠0
   - no sustained overcurrent / grinding chatter
7. If currents explode: `control stop`, IqVar=0, re-check Vdc≥40, then retry at 1 A.

## Higher bus / higher speed (~100 V)

1. Confirm `cg_vdc_v` ≈ 100 and calibrated `PsiF` / `Ld` / `Lq`.
2. Keep **Mode = 3**. Firmware allows ωe up to ±3000 rad/s.
3. Raise `IqVar` gradually (still slew-limited), up to `I_Max`.
4. Expect duties to leave mid-rail more as speed rises.

## If something trips

- Re-flash `foc_demo.json` if you need a known-good baseline.
- Confirm Sign matches the FOC run that worked.
- Lower `IqVar` / keep `I_Max` conservative.
- Keep `Mode = 3`. Never bring up Gen6 on Modes 0–2.
