#!/usr/bin/env bash
#
# Build a patched dfu-util for STM32G4 ROM DFU flashing.
#
# The stock dfu-util 0.11 (and the current git tree) treat USB pipe stalls
# and transient IO errors from the STM32 bootloader as hard failures while
# polling status after erase/set-address/download commands. This causes
# flashing to fail on larger firmware images with errors like:
#
#   Error during special command "ERASE_PAGE" get_status
#   Error during special command "SET_ADDRESS" get_status
#   Error during download get_status
#
# This script clones dfu-util, applies dfu-util-stm32g4-retry.patch, and
# builds it.  flash-dfu.sh will automatically prefer the resulting binary.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DFU_UTIL_DIR="${SCRIPT_DIR}/dfu-util-src"
PATCH_FILE="${SCRIPT_DIR}/dfu-util-stm32g4-retry.patch"

if [[ ! -f "${PATCH_FILE}" ]]; then
    echo "ERROR: Patch file not found: ${PATCH_FILE}" >&2
    exit 1
fi

# Install build dependencies if missing (Debian/Ubuntu).
if ! pkg-config --exists libusb-1.0 || ! command -v autoreconf &>/dev/null; then
    echo "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y build-essential libusb-1.0-0-dev autoconf automake libtool pkg-config
fi

if [[ -d "${DFU_UTIL_DIR}" ]]; then
    echo "Using existing ${DFU_UTIL_DIR}"
else
    echo "Cloning dfu-util source..."
    git clone --depth 1 https://git.code.sf.net/p/dfu-util/dfu-util "${DFU_UTIL_DIR}"
fi

cd "${DFU_UTIL_DIR}"

# Re-apply the patch in case the source tree was freshly cloned.
if git apply --check "${PATCH_FILE}" 2>/dev/null; then
    echo "Applying patch..."
    git apply "${PATCH_FILE}"
else
    echo "Patch already applied or does not apply cleanly, continuing..."
fi

./autogen.sh
./configure
make -j"$(nproc)"

echo ""
echo "Patched dfu-util built: ${DFU_UTIL_DIR}/src/dfu-util"
