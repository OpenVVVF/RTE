#!/usr/bin/env bash
# Local FOC vs MPC paper comparison (does NOT modify RTE git repository).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export MPLCONFIGDIR="$ROOT/results/.mplconfig"
mkdir -p "$MPLCONFIGDIR"

echo "== Build =="
cmake -B build -G Ninja >/dev/null
cmake --build build -j8 --target paper_foc_vs_mpc

echo "== Run comparison =="
./build/Lib/Simulation/paper_foc_vs_mpc

echo "== Plot =="
python3 scripts/plot_paper_foc_vs_mpc.py

echo
echo "Results:"
echo "  $ROOT/results/paper_foc_vs_mpc/metrics_summary.csv"
echo "  $ROOT/results/paper_foc_vs_mpc/plots/"
