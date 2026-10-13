#!/usr/bin/env bash
# Build and run the standalone hybrid ADRC+MPCC package.
# Does not modify or rebuild the parent Simulation targets.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/../../.." && pwd)"
BUILD="$ROOT/build"
OUT="$REPO/results/adrc_mpcc_hybrid"

mkdir -p "$BUILD" "$OUT"

cmake -S "$ROOT" -B "$BUILD" -G Ninja
cmake --build "$BUILD" -j8

if [[ "${1:-}" == "--test" ]]; then
  if [[ -x "$BUILD/test_adrc_mpcc_hybrid" ]]; then
    ctest --test-dir "$BUILD" --output-on-failure
  else
    echo "Unit test binary not built (GTest missing). Sim-only OK."
  fi
fi

"$BUILD/adrc_mpcc_hybrid_sim" "$OUT"

if command -v python3 >/dev/null 2>&1; then
  MPLCONFIGDIR="$OUT/.mplconfig" mkdir -p "$OUT/.mplconfig"
  MPLCONFIGDIR="$OUT/.mplconfig" python3 "$ROOT/plot_hybrid.py" "$OUT" || true
fi

echo ""
echo "Results in: $OUT"
ls -la "$OUT"
