#!/usr/bin/env bash
#
# Clean and rebuild the firmware image from scratch.
#
# Usage: ./rebuild.sh [Debug|Release]   (default: Debug)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRESET="${1:-Debug}"
BUILD_DIR="${SCRIPT_DIR}/build/${PRESET}"

case "${PRESET}" in
    Debug|Release) ;;
    *)
        echo "ERROR: Unknown preset '${PRESET}' (expected Debug or Release)" >&2
        exit 1
        ;;
esac

echo "Removing ${BUILD_DIR}..."
rm -rf "${BUILD_DIR}"

echo "Configuring (${PRESET})..."
cmake --preset "${PRESET}" "${SCRIPT_DIR}"

echo "Building..."
cmake --build --preset "${PRESET}"

echo ""
echo "Firmware image built:"
find "${BUILD_DIR}" -maxdepth 1 -name '*.elf' -o -maxdepth 1 -name '*.bin' -o -maxdepth 1 -name '*.hex' 2>/dev/null
