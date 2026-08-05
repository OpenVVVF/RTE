#!/usr/bin/env bash
# Linux HostSim live launcher.
# Emits the requested graph into HostSim, builds it, and starts the live TCP server.
# Defaults to the SPWM demo graph for backward compatibility with the demo menu.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTSIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${HOSTSIM_ROOT}/../.." && pwd)"

NO_GUI=0
FORCE_EMIT=0
GRAPH=""
SCENARIO=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-gui) NO_GUI=1 ;;
    --force-emit) FORCE_EMIT=1 ;;
    --graph)
      GRAPH="${2:-}"
      shift
      ;;
    --scenario)
      SCENARIO="${2:-}"
      shift
      ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

if [[ -z "${GRAPH}" ]]; then
  GRAPH="${HOSTSIM_ROOT}/graphs/spwm_demo_graph.json"
fi

if [[ ! -f "${GRAPH}" ]]; then
  echo "Graph not found: ${GRAPH}" >&2
  exit 1
fi

GRAPH_NAME="$(basename "${GRAPH}" .json)"

if [[ -z "${SCENARIO}" ]]; then
  # Prefer a scenario that matches the graph name (stripping a trailing _graph suffix);
  # fall back to the generic motor scenario.
  SCENARIO_BASE="${GRAPH_NAME}"
  if [[ "${SCENARIO_BASE}" == *_graph ]]; then
    SCENARIO_BASE="${SCENARIO_BASE%_graph}"
  fi
  CANDIDATE="${HOSTSIM_ROOT}/scenarios/${SCENARIO_BASE}.json"
  if [[ -f "${CANDIDATE}" ]]; then
    SCENARIO="${CANDIDATE}"
  else
    SCENARIO="${HOSTSIM_ROOT}/scenarios/default_motor.json"
  fi
fi

if [[ ! -f "${SCENARIO}" ]]; then
  echo "Scenario not found: ${SCENARIO}" >&2
  exit 1
fi

EMITTER="${RTE_EMITTER:-${REPO_ROOT}/build/Source/RTECodeEmitter/RTECodeEmitter}"
NODEGUI="${REPO_ROOT}/build/Source/NodeGUI/NodeGUI"

if [[ ! -x "${EMITTER}" ]]; then
  echo "RTECodeEmitter not found at ${EMITTER}" >&2
  echo "Build host tools: cmake -S . -B build && cmake --build build --target RTECodeEmitter" >&2
  exit 1
fi

EMITTED_REL="build/hostsim_${GRAPH_NAME}_emitted"
BUILD_DIR="${REPO_ROOT}/${EMITTED_REL}_build"

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

  echo "Emitting ${GRAPH_NAME} graph into HostSim..."
  "${EMITTER}" \
    --base-src "${HOSTSIM_ROOT}" \
    --templatesDir "${REPO_ROOT}/Assets/NodeTemplates" \
    --graph "${GRAPH}" \
    --output "${EMITTED_REL}" \
    --verbosity info

  cmake -S "${REPO_ROOT}/${EMITTED_REL}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
else
  echo "Using existing emitted ${GRAPH_NAME} build (pass --force-emit to rebuild)."
fi

if [[ ! -x "${EXE}" ]]; then
  echo "host_sim not found in ${BUILD_DIR}" >&2
  exit 1
fi

echo "Starting HostSim live for ${GRAPH_NAME}..."
nohup "${EXE}" "${SCENARIO}" --live --realtime 1.0 >/dev/null 2>&1 &

if [[ "${NO_GUI}" -eq 0 ]]; then
  if [[ ! -x "${NODEGUI}" ]]; then
    echo "NodeGUI not found at ${NODEGUI}. Build with: cmake --build build --target NodeGUI" >&2
    echo "HostSim is still running."
    exit 0
  fi
  sleep 1
  nohup "${NODEGUI}" "${GRAPH}" --tcp 127.0.0.1:14608 --protocol ivp >/dev/null 2>&1 &
  echo "NodeGUI opened with ${GRAPH_NAME} graph + live telemetry."
fi

echo ""
echo "Scenario: ${SCENARIO}"
echo "Live telemetry: 127.0.0.1:14608 (IVP)"
