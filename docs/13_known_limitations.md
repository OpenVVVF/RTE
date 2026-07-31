# Known Limitations

1. **No RTE plant model upstream** — `Lib/Simulation` provides PMSM/inverter models for this project only.
2. **RTE hardware inverter is duty-based** — Modes 0–2 use Sa/Sb/Sc → `Transforms.SwitchToDuty`. Mode 3 outputs continuous Vαβ → `Transforms.Svpwm` (same path as FOC).
3. **Ideal inverter** — no dead time, device drops, or DC-link ripple in simulation.
4. **Forward-Euler plant** — predictor/plant mismatch causes nonzero one-step prediction error (reported in tests, not bit-exact).
5. **Conventional FCS-MPCC (Mode 0–2)** — unsuitable for Gen6 low-L motors (~100 µH): one full active vector at Ts=200 µs predicts Δi≈V·Ts/L≈10³ A, so the cost always picks zero-vector S000/S111 (duties look stuck at 0% or 100%). Use **Mode 3** (deadbeat voltage → SVPWM) on hardware.
6. **Speed loop** — simple PI on ω_m → i_q*; not tuned for production.
7. **Back-EMF estimation** in host `MpccController` uses simplified dq proxy; Zhang paper uses full αβ stationary formulation.
8. **Improved FCS modes** implemented and selectable; Gen6 spin path is Mode 3.
9. **STM32 timing** — MPC execution ~0.4–0.7 µs mean on host; MCU budget not yet validated under load.

## Planned STM32 implementation

1. Validate Mode 3 spin on Gen6 with the same encoder Sign / offset / poles as working `foc_demo`.
2. Optionally re-introduce true FCS (Mode 0) at shorter Ts / higher PWM if needed for the paper.
3. Port host `MpccController` refinements if firmware Mode 3 needs Zhang Method II duty optimization.
4. Compare against RTE `foc_demo.json` PI+SVPWM baseline.
