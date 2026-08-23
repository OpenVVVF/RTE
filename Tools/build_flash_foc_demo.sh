#!/usr/bin/env bash
set -euo pipefail

# Build the FOC demo graph into a flashable STM32 binary and optionally live-flash
# it through the unified rte automation backend.
#
# Usage:
#   Tools/build_flash_foc_demo.sh [options]
#
# Options:
#   --no-flash          Build only; do not attempt to flash.
#   --flash-only        Skip build; only flash the existing binary.
#   --serial <port>     Flash directly through this port; otherwise use RTE Studio.
#   --build-type <type> Debug | Release | RelWithDebInfo | MinSizeRel (default: Release).
#              NOTE: Debug (-O0) starves the CPU under full control ISR load
#              (hz_app_loop collapses from ~9 kHz to <100 Hz); only use it
#              when you actually need a debugger.
#   --clean             Wipe emitted source and build dirs before building.
#   --dry-run           Print the rte command without executing.
#   -h, --help          Show this message.
#
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${PROJECT_ROOT}/build"
FW_BUILD_DIR="${BUILD_DIR}/foc-demo-fw"
FW_SRC_DIR="${BUILD_DIR}/foc-demo-fw-src"
BASE_SRC="${PROJECT_ROOT}/Images/Gen6FW"
GRAPH="${PROJECT_ROOT}/Assets/Examples/foc_demo.json"

BIN_FILE="${FW_BUILD_DIR}/STM32CubeMX.bin"

NO_FLASH=0
FLASH_ONLY=0
CLEAN=0
DRY_RUN=0
BUILD_TYPE="Release"
SERIAL_PORT=""

usage() {
    sed -n '4,21p' "$0" | sed 's/^# //' | sed 's/^#//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-flash)
            NO_FLASH=1
            shift
            ;;
        --flash-only)
            FLASH_ONLY=1
            shift
            ;;
        --flash-url)
            echo "Warning: --flash-url is deprecated; RTE Studio is discovered automatically." >&2
            shift 2
            ;;
        --serial)
            SERIAL_PORT="$2"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            NO_FLASH=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

ensure_tools_built() {
    local builder="${BUILD_DIR}/bin/rte"
    local emitter="${BUILD_DIR}/bin/rte"

    if [[ -x "$builder" && -x "$emitter" ]]; then
        return 0
    fi

    echo "RTE CLI not found; building rte ..."
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target rte --parallel
}

build_firmware() {
    ensure_tools_built

    local builder="${BUILD_DIR}/bin/rte"
    local -a args=(
        build
        --build-dir "${FW_BUILD_DIR}"
        --graph "${GRAPH}"
        --base-source "${BASE_SRC}"
        --source-output "${FW_SRC_DIR}"
        --build-type "${BUILD_TYPE}"
    )

    if [[ "$CLEAN" -eq 1 ]]; then
        args+=(--clean)
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf 'Dry run:'
        printf ' %q' "${builder}" "${args[@]}"
        printf '\n'
        return
    fi

    echo ""
    echo "Building FOC demo firmware ..."
    echo "  Graph:  ${GRAPH}"
    echo "  Output: ${FW_BUILD_DIR}"
    echo ""

    "${builder}" "${args[@]}"
}

flash_firmware() {
    if [[ ! -f "${BIN_FILE}" ]]; then
        echo "Error: firmware binary not found at ${BIN_FILE}" >&2
        exit 1
    fi

    ensure_tools_built
    local -a args=(flash --firmware "${BIN_FILE}")
    if [[ -n "${SERIAL_PORT}" ]]; then args+=(--serial "${SERIAL_PORT}"); fi
    "${BUILD_DIR}/bin/rte" "${args[@]}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [[ "$FLASH_ONLY" -eq 0 ]]; then
    build_firmware
fi

if [[ "$NO_FLASH" -eq 0 ]]; then
    flash_firmware
else
    echo ""
    echo "Build complete; flash skipped."
    echo "Binary: ${BIN_FILE}"
fi
