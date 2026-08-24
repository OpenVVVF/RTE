#!/usr/bin/env bash
set -euo pipefail

# Flash CoProcessor firmware via DFU.
# Usage: ./flash-coproc.sh [firmware.elf]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STM32_PROGRAMMER="${STM32_PROGRAMMER:-/home/tliao/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"
FIRMWARE="${1:-${SCRIPT_DIR}/build/Debug/CoProcessor.elf}"

if [[ ! -f "${FIRMWARE}" ]]; then
    echo "ERROR: Firmware not found: ${FIRMWARE}" >&2
    exit 1
fi

echo "==> Flashing CoProcessor: ${FIRMWARE}"
"${STM32_PROGRAMMER}" -c port=usb1 -d "${FIRMWARE}" -v -g 0x08000000

echo "==> Done. CoProcessor should be running."
