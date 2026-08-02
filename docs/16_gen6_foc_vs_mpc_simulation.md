# Gen6 FOC vs MPC simulation study

Branch: `feature/gen6-foc-vs-mpc-simulation`  
Distinct from hardware bring-up branch `feature/fcs-mpcc-three-phase-pmsm`.

## What this adds

- Fixed FCS-MPCC optimal-duty law (`MpccController.h`) for synchronous simulation
- Gen6 bench motor parameter helper (`BenchMotor.h`)
- Paper-style comparison executable `paper_foc_vs_mpc`
- Scripts: `scripts/run_paper_foc_vs_mpc.sh`, `scripts/plot_paper_foc_vs_mpc.py`
- Example outputs under `results/paper_foc_vs_mpc/`

## Run

```bash
cmake -B build -G Ninja
cmake --build build -j8 --target paper_foc_vs_mpc
./scripts/run_paper_foc_vs_mpc.sh
```

Motor parameters are Gen6-bench-like (not Zhang Table I / not Li et al. machine data).
