#!/usr/bin/env bash
# Linux equivalent of run_spwm_live.ps1
# Emit SPWM graph, build HostSim, launch live TCP server + NodeGUI.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTSIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${HOSTSIM_ROOT}/../.." && pwd)"

NO_GUI=0
FORCE_EMIT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-gui) NO_GUI=1 ;;
    --force-emit) FORCE_EMIT=1 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

GRAPH="${HOSTSIM_ROOT}/graphs/spwm_demo_graph.json"
SCENARIO="${HOSTSIM_ROOT}/scenarios/spwm_demo.json"
EMITTER="${RTE_EMITTER:-${REPO_ROOT}/build/Source/RTECodeEmitter/RTECodeEmitter}"
NODEGUI="${REPO_ROOT}/build/Source/NodeGUI/NodeGUI"

if [[ ! -x "${EMITTER}" ]]; then
  echo "RTECodeEmitter not found at ${EMITTER}" >&2
  echo "Build host tools: cmake -S . -B build && cmake --build build --target RTECodeEmitter" >&2
  exit 1
fi

EMITTED_REL="build/hostsim_spwm_emitted"
BUILD_DIR="${REPO_ROOT}/build/hostsim_spwm_emitted_build"

cleanup_apps() {
  # Use exact-name matching so we do not kill the shell running this script
  # or the NodeGUI instance that launched us via Build -> Build Simulation.
  pgrep -x host_sim | xargs -r kill 2>/dev/null || true
  sleep 0.5
}

cleanup_apps

EXE="${BUILD_DIR}/host_sim"
need_emit=0
if [[ "${FORCE_EMIT}" -eq 1 ]] || [[ ! -x "${EXE}" ]]; then
  need_emit=1
elif [[ "${GRAPH}" -nt "${EXE}" ]]; then
  echo "Graph newer than emitted build - re-emitting..."
  need_emit=1
fi

if [[ "${need_emit}" -eq 1 ]]; then
  rm -rf "${REPO_ROOT}/${EMITTED_REL}" "${BUILD_DIR}"

  echo "Emitting SPWM graph..."
  "${EMITTER}" \
    --base-src "${HOSTSIM_ROOT}" \
    --graph "${GRAPH}" \
    --output "${EMITTED_REL}" \
    --verbosity info

  cmake -S "${REPO_ROOT}/${EMITTED_REL}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
else
  echo "Using existing emitted SPWM build (pass --force-emit to rebuild)."
fi

if [[ ! -x "${EXE}" ]]; then
  echo "host_sim not found in ${BUILD_DIR}" >&2
  exit 1
fi

echo "Starting HostSim SPWM live..."
nohup "${EXE}" "${SCENARIO}" --live --realtime 1.0 >/dev/null 2>&1 &

if [[ "${NO_GUI}" -eq 0 ]]; then
  if [[ ! -x "${NODEGUI}" ]]; then
    echo "NodeGUI not found at ${NODEGUI}. Build with: cmake --build build --target NodeGUI" >&2
    echo "HostSim is still running."
    exit 0
  fi
  sleep 1
  nohup "${NODEGUI}" "${GRAPH}" --tcp 127.0.0.1:14608 --protocol ivp >/dev/null 2>&1 &
  echo "NodeGUI opened with SPWM graph + live telemetry."
fi

echo ""
echo "Live controls:"
echo "  Throttle A -> modulation index (0..1)"
echo "  Throttle B -> electrical frequency map (1..20 Hz)"
echo "Plot: duty_u/v/w (slow), pwm_gate_u/v/w + pwm_v_uv (scope), i_a/b/c"
