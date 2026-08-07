#!/usr/bin/env bash
# Run an accurate ngspice batch render for a user-specified time window.
# Choose tim_isr_hz >= 20 * f_elec_hz * points_per_cycle for ~20th harmonic clarity.
#
# Usage:
#   ./render_spice.sh [duration_s] [tim_isr_hz] [substeps] [scenario]
set -euo pipefail

DURATION="${1:-0.005}"
TIM_ISR_HZ="${2:-50000}"
SUBSTEPS="${3:-1}"
SCENARIO="${4:-scenarios/accurate_spice.json}"
TRACE="accurate_spice_trace.csv"

HOSTSIM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HOSTSIM_ROOT/build/host_sim"
if [[ ! -x "$EXE" && -x "$HOSTSIM_ROOT/build/Debug/host_sim.exe" ]]; then
    EXE="$HOSTSIM_ROOT/build/Debug/host_sim.exe"
fi

echo "Rendering accurate SPICE window: ${DURATION} s at ${TIM_ISR_HZ} Hz ISR, ${SUBSTEPS} substeps"
echo "Trace: ${TRACE}"

cd "$HOSTSIM_ROOT"
"$EXE" "$SCENARIO" \
    --plant-backend ngspice \
    --duration "$DURATION" \
    --tim-isr-hz "$TIM_ISR_HZ" \
    --substeps "$SUBSTEPS"
echo "Done. Plot with: python scripts/plot_sim.py $TRACE"
