#!/usr/bin/env bash
set -euo pipefail

# Emit the SIL firmware tree: a copy of Images/Gen6FW with the node graph's
# generated domain code in generated/ (identical mechanism to the ARM build;
# only the compile step differs).  Run this to regenerate after changing the
# graph, or before the first HostSIL CMake configure (it also auto-emits
# when missing).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTSIL_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${HOSTSIL_ROOT}/../.." && pwd)"

GRAPH="${1:-${REPO_ROOT}/Assets/Examples/foc_demo.json}"
OUTPUT="${2:-${REPO_ROOT}/build/hostsil_fw_src}"

EMITTER="${RTE_EMITTER:-${REPO_ROOT}/build/bin/RTECodeEmitter}"
if [[ ! -x "${EMITTER}" ]]; then
  echo "RTECodeEmitter not found at ${EMITTER}" >&2
  echo "Build host tools: cmake -B build && cmake --build build --target RTECodeEmitter" >&2
  exit 1
fi

rm -rf "${OUTPUT}"
"${EMITTER}" \
  --base-src "${REPO_ROOT}/Images/Gen6FW" \
  --graph "${GRAPH}" \
  --output "${OUTPUT}" \
  --verbosity warning

echo "SIL firmware tree emitted: ${OUTPUT} (graph: ${GRAPH})"
