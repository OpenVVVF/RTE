#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RTE_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

GRAPH="${1:-${RTE_ROOT}/Assets/Examples/foc_demo.json}"
BUILD_TYPE="${2:-Release}"
SOURCE_OUTPUT="${SCRIPT_DIR}/.rte-gen7/generated"
BUILD_DIR="${SCRIPT_DIR}/.rte-gen7/build/${BUILD_TYPE}"

case "${BUILD_TYPE}" in
  Debug|Release|RelWithDebInfo|MinSizeRel) ;;
  *)
    echo "Usage: $0 [graph.json] [Debug|Release|RelWithDebInfo|MinSizeRel]" >&2
    exit 2
    ;;
esac

if [[ ! -f "${GRAPH}" ]]; then
  echo "Error: graph not found: ${GRAPH}" >&2
  exit 2
fi

if command -v rte >/dev/null 2>&1; then
  RTE_BIN="$(command -v rte)"
elif [[ -x "${RTE_ROOT}/build/bin/rte" ]]; then
  RTE_BIN="${RTE_ROOT}/build/bin/rte"
else
  echo "Error: rte was not found in PATH or at ${RTE_ROOT}/build/bin/rte" >&2
  echo "Build the RTE host tools from ${RTE_ROOT}, then run this script again." >&2
  exit 127
fi

echo "==> RTE:        ${RTE_BIN}"
echo "==> Base image: ${SCRIPT_DIR}/MainProcessor"
echo "==> Graph:      ${GRAPH}"
echo "==> Build type: ${BUILD_TYPE}"

"${RTE_BIN}" build \
  --graph "${GRAPH}" \
  --base-source "${SCRIPT_DIR}/MainProcessor" \
  --source-output "${SOURCE_OUTPUT}" \
  --build-dir "${BUILD_DIR}" \
  --build-type "${BUILD_TYPE}"

ELF="${BUILD_DIR}/STM32CubeMX.elf"
BIN="${BUILD_DIR}/STM32CubeMX.bin"

if [[ ! -f "${ELF}" ]]; then
  echo "Error: RTE reported success but did not produce ${ELF}" >&2
  exit 1
fi

echo "==> Gen7 firmware build complete"
echo "    ELF: ${ELF}"
[[ -f "${BIN}" ]] && echo "    BIN: ${BIN}"
