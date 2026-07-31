# Traceability Table

| Technical element | Source | Paper equation / RTE file | New implementation file | Test |
|-------------------|--------|---------------------------|---------------------------|------|
| Clarke transform | RTE convention | `math.clarke/inline.cpp` | `Lib/Simulation/include/simulation/Transforms.h` | implicit in PMSM/MPCC tests |
| Park transform | RTE convention | `math.park/inline.cpp` | `Transforms.h` | `Simulation_mpcc_tests` |
| Inverse Park | RTE convention | `math.inverse_park/inline.cpp` | `Transforms.h` | MPCC node outputs |
| SVPWM (averaged) | RTE hardware path | `math.svpwm/inline.cpp` | — (not used by FCS-MPCC) | `TwoLevelInverter.CompareWithRteSvpwmLinearLimit` |
| 8-state VSI αβ voltage | Standard 2-level VSI | Zhang context / user spec | `TwoLevelInverter.h` | `Simulation_inverter_tests` (8 tests) |
| PMSM dq voltage equations | Standard dq model | User Stage 3 | `PmsmPlant.h` | `Simulation_pmsm_tests` (9 tests) |
| PMSM torque equation | Standard dq model | User Stage 3 | `PmsmPlant.h` | `SpmTorqueVersusIq` |
| Conventional FCS-MPCC | Zhang et al. baseline | Stage 6 / eq. discretized dq | `MpccController.h` | `Simulation_mpcc_tests` |
| Normalized cost | User Stage 6 | — | `MpccController::normalizedCost` | `CostCalculation` |
| Current limit penalty | User Stage 6 | — | `MpccController.h` | `CurrentLimitPenalty` |
| Tie-breaking | User Stage 6 | — | `MpccController.h` | `TieBreakingPrefersFewerTransitions` |
| Delay compensation | Zhang eq. (11)-(12) | Sec. III-B | `MpccController::delayCompensatedCurrent` | mode selectable |
| Back-EMF estimation | Zhang eq. (7)-(10) | Sec. III-A | `MpccController::estimateEmfComponent` | mode selectable |
| Optimal duty (Method II) | Zhang eq. (18)-(19) | Sec. III-C | `MpccController::evaluateOptimalDuty` | mode selectable |
| RTE MPC node (firmware) | This project | — | `Assets/NodeTemplates/control.mpcc/` | codegen manual |
| Closed-loop simulation | This project | — | `ClosedLoopSimulator.h`, `mpcc_closed_loop.cpp` | CSV + plots |

## Reused from RTE (unchanged logic)

- Node template structure and codegen pipeline
- Transform sign conventions from graph nodes
- Motor parameters default: Zhang et al. Table I (same as RTE `reminder.md` bench motor order of magnitude)
- Build/test infrastructure (`NodeAPI`, `InverterCodegen`, etc.)

## New code written in `MPC_Three-phase_PMSM` only

- `Lib/Simulation/**`
- `Assets/NodeTemplates/control.mpcc/**`
- `docs/**`
- `scripts/plot_results.py`
- `results/**`

## Engineering assumptions

1. FCS-MPCC uses **dq prediction** (user Stage 6), not Zhang's αβ stationary model.
2. Plant uses forward-Euler integration with `Ts = 100 µs`.
3. Inverter model is **ideal** (no dead time, no device drops) for simulation.
4. RTE hardware inverter remains duty-based; `control.mpcc` outputs switching states for future adapter to PWM.
