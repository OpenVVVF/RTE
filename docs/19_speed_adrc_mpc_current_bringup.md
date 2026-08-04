# Speed ADRC + MPC Current (cascaded Gen6)

**New** branch / demo — does **not** modify `Control.AdrcMpcc` or `hybrid_adrc_mpcc_demo.json`.

```
Spd_Ref [rpm]  →  LADRC (speed)  →  Iq*
Id_Ref [A]     →                     ↘
I_Q_Ref [A]    →  (CascadeMode=0 only) →  Mode-3 deadbeat / MPC current  →  Vαβ → SVPWM
```

| Mode | `CascadeMode` | How to command |
|------|---------------|----------------|
| Current-only (FOC-like) | **0** | `IqVar` in **amps** |
| Cascaded (this design) | **1** (demo default) | `SpdVar` in **rpm**; current loop still runs |

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

## Recommended first run (cascade)

1. Bus **~55 V**
2. Encoder Sign/Offset same as FOC
3. `CascadeMode = 1`, `IdVar = 0`, `SpdVar = 400` (rpm)
4. Enable → expect rpm → ~400, phase currents modest, `sam_iq_cmd_a` / `sam_rpm` in telem
5. Optional: 400 → 500 rpm. Stay ≤ ~600 at 55 V

## FOC-like current-only check

Set `CascadeMode = 0`, ramp `IqVar` 1→3→5 A (same idea as FOC / old hybrid current test).

## DTCM note

Do not add a graph `Control.Slew` on `Spd_Ref` — that overflowed DTCM by ~16 B. Rpm slew is inside the controller (AXISRAM).

## Tuning

| Param | Default | Role |
|-------|---------|------|
| `SpdOmegaC` | 40 | Speed ADRC ωc |
| `SpdOmegaO` | 120 | Speed ADRC ωo |
| `SpdB0` | 80 | rpm/s per amp (tune) |
| `DbScale` | 0.55 | Inner MPC deadbeat |
| `I_Max` | 15 | Hard current clamp |
| `PsiF` | 0.07 | Inner bemf model |

## vs previous hybrid

| | `hybrid_adrc_mpcc_demo` | This demo |
|--|-------------------------|-----------|
| Node | `Control.AdrcMpcc` | `Control.SpeedAdrcMpcCurrent` |
| “Hybrid” meaning | ESO+deadbeat in **one** current law | **ADRC speed** + **MPC current** cascade |
| Untouched | — | Previous hybrid files left as-is |
