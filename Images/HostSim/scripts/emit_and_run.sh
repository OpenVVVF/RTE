#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTSIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${HOSTSIM_ROOT}/../.." && pwd)"
GRAPH="${1:-${HOSTSIM_ROOT}/baseline_graph.json}"

EMITTER="${RTE_EMITTER:-${REPO_ROOT}/build/Source/RTECodeEmitter/RTECodeEmitter}"
if [[ ! -x "${EMITTER}" ]]; then
  echo "RTECodeEmitter not found at ${EMITTER}" >&2
  echo "Build host tools: cmake -B build && cmake --build build --target RTECodeEmitter" >&2
  exit 1
fi

rm -rf "${REPO_ROOT}/build/hostsim_emitted"
"${EMITTER}" \
  --base-src "${HOSTSIM_ROOT}" \
  --graph "${GRAPH}" \
  --output "${REPO_ROOT}/build/hostsim_emitted" \
  --verbosity info

BUILD_DIR="${REPO_ROOT}/build/hostsim_emitted_build"
rm -rf "${BUILD_DIR}"
cmake -S "${REPO_ROOT}/build/hostsim_emitted" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}"

"${BUILD_DIR}/host_sim" "${REPO_ROOT}/build/hostsim_emitted/scenarios/default_motor.json"
echo "Emit-and-run complete: ${BUILD_DIR}/host_sim"
