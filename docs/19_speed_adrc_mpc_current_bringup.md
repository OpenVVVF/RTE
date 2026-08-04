# Speed ADRC + MPC Current (cascaded Gen6)

**New** branch / demo — does **not** modify `Control.AdrcMpcc` or `hybrid_adrc_mpcc_demo.json`.

```
SpdVar [rpm]  →  LADRC (speed)  →  Iq*  →  Mode-3 deadbeat / MPC current  →  Vαβ → SVPWM
IdVar [A]     →  (usually 0)
```

This controller is **always cascaded** (no `CascadeVar`). A mode `Values.Var` overflowed DTCM by ~16 B.

For FOC-like **Iq in amps** tests, use `hybrid_adrc_mpcc_demo.json` / `Control.AdrcMpcc`.

## Flash

```bash
cd "/Users/saeed/Desktop/Journal Paper 2026/GitHub Repository/RTE"
git checkout feature/gen6-speed-adrc-mpc-current

cmake -B build -G Ninja && cmake --build build -j8

./Tools/build_flash_graph.sh \
  --graph Assets/Examples/speed_adrc_mpc_current_demo.json \
  --fw-build-dir build/speed-adrc-mpc-fw \
  --fw-src-dir build/speed-adrc-mpc-fw-src \
  --clean
```

## Test knobs

| Knob | Value | Notes |
|------|--------|------|
| **SpdVar** | **300** then 400 | rpm (own GUI position, not on top of IqVar) |
| **IdVar** | **0** | |
| **IqVar** | **0** | unused in this demo |
| Enable | on | |
| Vdc | ~55 V | stay ≤ ~600 rpm |

Telem: `sam_rpm`, `sam_iq_cmd_a`, phase currents, `hz_tim_isr`.

## DTCM rules

- Controller state → AXISRAM (`.dma_buffers`)
- **Do not** add `Control.Slew` on Spd_Ref or a `CascadeVar` — each costs ~16 B DTCM
