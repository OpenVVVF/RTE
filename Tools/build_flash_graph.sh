#!/usr/bin/env bash
set -euo pipefail

# Build a node graph into a flashable STM32 binary and optionally live-flash
# it through rte-gateway. The FOC demo is the default
# graph for convenient command-line use.
#
# Usage:
#   Tools/build_flash_graph.sh [options]
#
# Options:
#   --graph <path>      Graph JSON to build (default: Assets/Examples/foc_demo.json).
#   --fw-build-dir <d> Firmware CMake build directory (default: build/foc-demo-fw).
#   --fw-src-dir <d>   Emitted firmware source directory (default: build/foc-demo-fw-src).
#   --no-flash          Build only; do not attempt to flash.
#   --flash-only        Skip build; only flash the existing binary.
#   --flash-url <url>   Override the live-flash client URL.
#   --flash-lease <id>  Use an existing gateway operator lease.
#   --build-type <type> Debug | Release | RelWithDebInfo | MinSizeRel (default: Release).
#              NOTE: Debug (-O0) starves the CPU under full control ISR load
#              (hz_app_loop collapses from ~9 kHz to <100 Hz); only use it
#              when you actually need a debugger.
#   --clean             Wipe emitted source and build dirs before building.
#   --dry-run           Print RTEFirmwareBuilder commands without executing.
#   -h, --help          Show this message.
#
# Environment:
#   RTE_GATEWAY_URL             Base URL of the running rte-gateway
#   INVERTER_CLIENT_FLASH_URL   Deprecated fallback for RTE_GATEWAY_URL
#                               (default: http://localhost:18080).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${PROJECT_ROOT}/build"
FW_BUILD_DIR=""
FW_SRC_DIR=""
BASE_SRC="${PROJECT_ROOT}/Images/Gen6FW"
GRAPH="${PROJECT_ROOT}/Assets/Examples/foc_demo.json"

NO_FLASH=0
FLASH_ONLY=0
CLEAN=0
DRY_RUN=0
BUILD_TYPE="Release"
CLIENT_FLASH_URL="${RTE_GATEWAY_URL:-${INVERTER_CLIENT_FLASH_URL:-http://localhost:18080}}"
FLASH_LEASE=""

usage() {
    sed -n '4,28p' "$0" | sed 's/^# //' | sed 's/^#//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --graph)
            GRAPH="$2"
            shift 2
            ;;
        --fw-build-dir)
            FW_BUILD_DIR="$2"
            shift 2
            ;;
        --fw-src-dir)
            FW_SRC_DIR="$2"
            shift 2
            ;;
        --no-flash)
            NO_FLASH=1
            shift
            ;;
        --flash-only)
            FLASH_ONLY=1
            shift
            ;;
        --flash-url)
            CLIENT_FLASH_URL="$2"
            shift 2
            ;;
        --flash-lease)
            FLASH_LEASE="$2"
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

FW_BUILD_DIR="${FW_BUILD_DIR:-${BUILD_DIR}/foc-demo-fw}"
FW_SRC_DIR="${FW_SRC_DIR:-${BUILD_DIR}/foc-demo-fw-src}"
BIN_FILE="${FW_BUILD_DIR}/STM32CubeMX.bin"

if [[ ! -f "${GRAPH}" ]]; then
    echo "Error: graph file not found at ${GRAPH}" >&2
    exit 1
fi

ensure_tools_built() {
    local builder="${BUILD_DIR}/Source/RTEFirmwareBuilder/RTEFirmwareBuilder"
    local emitter="${BUILD_DIR}/Source/RTECodeEmitter/RTECodeEmitter"
    local ctl="${BUILD_DIR}/Source/RTECtl/rtectl"

    if [[ -x "$builder" && -x "$emitter" && -x "$ctl" ]]; then
        return 0
    fi

    echo "RTE build tools not found; building RTEFirmwareBuilder and RTECodeEmitter ..."
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target RTEFirmwareBuilder RTECodeEmitter rtectl --parallel
}

build_firmware() {
    ensure_tools_built

    local builder="${BUILD_DIR}/Source/RTEFirmwareBuilder/RTEFirmwareBuilder"
    local -a args=(
        --fw-src "${BASE_SRC}"
        --build-dir "${FW_BUILD_DIR}"
        --graph "${GRAPH}"
        --base-src "${BASE_SRC}"
        --output "${FW_SRC_DIR}"
        --build-type "${BUILD_TYPE}"
    )

    if [[ "$CLEAN" -eq 1 ]]; then
        args+=(--clean)
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        args+=(--dry-run)
    fi

    echo ""
    echo "Building graph firmware ..."
    echo "  Graph:  ${GRAPH}"
    echo "  Output: ${FW_BUILD_DIR}"
    echo ""

    "${builder}" "${args[@]}"
}

flash_via_client() {
    echo "Gateway detected at ${CLIENT_FLASH_URL}"
    echo "Uploading and flashing ${BIN_FILE} ..."
    local -a ctl_args=(--server "${CLIENT_FLASH_URL}")
    if [[ -n "${FLASH_LEASE}" ]]; then
        ctl_args+=(--lease "${FLASH_LEASE}")
    fi
    "${BUILD_DIR}/Source/RTECtl/rtectl" "${ctl_args[@]}" flash "${BIN_FILE}"
}

flash_firmware() {
    if [[ ! -f "${BIN_FILE}" ]]; then
        echo "Error: firmware binary not found at ${BIN_FILE}" >&2
        exit 1
    fi

    ensure_tools_built

    local probe
    probe=$("${BUILD_DIR}/Source/RTECtl/rtectl" --server "${CLIENT_FLASH_URL}" info 2>/dev/null || true)
    if [[ -z "$probe" ]]; then
        echo ""
        echo "No rte-gateway detected at ${CLIENT_FLASH_URL}."
        echo "Binary is ready: ${BIN_FILE}"
        echo "Start rte-gateway (or set RTE_GATEWAY_URL / --flash-url) and re-run."
        exit 0
    fi

    echo ""
    if flash_via_client; then
        echo ""
        echo "Flash complete."
    else
        echo ""
        echo "Flash failed." >&2
        exit 1
    fi
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
