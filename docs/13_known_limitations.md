# Known Limitations

1. **No RTE plant model upstream** — `Lib/Simulation` provides PMSM/inverter models for this project only.
2. **RTE hardware inverter is duty-based** — `Control.Mpcc` outputs Sa/Sb/Sc; a PWM adapter is needed for STM32 deployment.
3. **Ideal inverter** — no dead time, device drops, or DC-link ripple in simulation.
4. **Forward-Euler plant** — predictor/plant mismatch causes nonzero one-step prediction error (reported in tests, not bit-exact).
5. **Conventional FCS-MPCC** — high current THD in simulation (~48% steady-state on iq step) as expected for single-vector-per-period control.
6. **Speed loop** — simple PI on ω_m → i_q*; not tuned for production.
7. **Back-EMF estimation** in `MpccController` uses simplified dq proxy; Zhang paper uses full αβ stationary formulation.
8. **Improved modes** implemented and selectable; closed-loop scenarios run conventional mode by default.
9. **STM32 timing** — MPC execution ~0.4–0.7 µs mean on host; MCU budget not yet validated.

## Planned STM32 implementation

1. Wire `Control.Mpcc` into `tim_isr` domain graph.
2. Add switching-state → PWM adapter (or modify `PWM_SetThreePhaseDuty` path).
3. Port `MpccController` to fixed-point if needed.
4. Validate on Gen6FW hardware with `Motor.PMSM` calibrated parameters.
5. Compare against RTE `foc_demo.json` PI+SVPWM baseline.
