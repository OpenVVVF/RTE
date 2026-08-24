#!/usr/bin/env bash
#
# Flash the STM32G474 CoProcessor firmware over USB DFU using the
# STM32CubeProgrammer CLI (STM32_Programmer_CLI).
#
# Target: STM32G474RCTx (256 KiB dual-bank Flash, from CoProcessor.ioc)
#   Bank 1: 0x0800 0000 - 0x0801 FFFF (128 KiB)
#   Gap:    0x0802 0000 - 0x0803 FFFF
#   Bank 2: 0x0804 0000 - 0x0805 FFFF (128 KiB)
#
# The ST ROM bootloader exposes the internal flash over USB DFU
# (VID:PID 0483:df11); STM32_Programmer_CLI talks to it via "-c port=usb1".
#
# References:
#   - ST AN2606 (system memory boot mode), STM32G47xxx/48xxx section
#   - ST UM2237 (STM32CubeProgrammer command-line interface)
#
# Usage:
#   ./flash-dfu.sh [path/to/firmware.elf|.hex|.bin]
#
# If no firmware path is given, the script flashes:
#   build/Debug/CoProcessor.elf
# Flashing the ELF directly means there is no stale .bin to worry about.
# A raw .bin (no embedded addresses) is written at 0x08000000.
#
# The board must be in STM32 ROM bootloader DFU mode before running this script.
# On many boards this is done by holding BOOT0 high while resetting, or by using
# a USB-PD / debug adapter that can request the bootloader.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_NAME="CoProcessor"
BUILD_PRESET="Debug"
FLASH_ADDR="0x08000000"
# Total flash size for STM32G474RCTx (256 KiB = 262144 bytes)
FLASH_SIZE_MAX=$((256 * 1024))

# Locate STM32_Programmer_CLI (override with the STM32_PROGRAMMER env var).
STM32_PROGRAMMER="${STM32_PROGRAMMER:-}"
if [[ -z "${STM32_PROGRAMMER}" ]]; then
    if command -v STM32_Programmer_CLI &>/dev/null; then
        STM32_PROGRAMMER="$(command -v STM32_Programmer_CLI)"
    else
        for candidate in \
            /usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
            "${HOME}/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI"
        do
            if [[ -x "${candidate}" ]]; then
                STM32_PROGRAMMER="${candidate}"
                break
            fi
        done
    fi
fi
if [[ -z "${STM32_PROGRAMMER}" ]]; then
    echo "ERROR: STM32_Programmer_CLI not found." >&2
    echo "Install STM32CubeProgrammer, or point STM32_PROGRAMMER at the binary:" >&2
    echo "  STM32_PROGRAMMER=/path/to/STM32_Programmer_CLI $0" >&2
    exit 1
fi
echo "Using STM32CubeProgrammer CLI: ${STM32_PROGRAMMER}"

FIRMWARE_FILE="${1:-${SCRIPT_DIR}/build/${BUILD_PRESET}/${PROJECT_NAME}.elf}"

# If no firmware was given and the default ELF is missing, try to build it.
if [[ ! -f "${FIRMWARE_FILE}" && $# -eq 0 ]]; then
    echo "Firmware not found: ${FIRMWARE_FILE}" >&2
    echo "Attempting CMake build (preset: ${BUILD_PRESET})..." >&2
    if ! command -v cmake &>/dev/null || ! command -v arm-none-eabi-gcc &>/dev/null; then
        echo "ERROR: CMake and/or arm-none-eabi-gcc are not available to build the firmware." >&2
        exit 1
    fi
    cmake --preset "${BUILD_PRESET}" -S "${SCRIPT_DIR}"
    cmake --build "${SCRIPT_DIR}/build/${BUILD_PRESET}"
fi

if [[ ! -f "${FIRMWARE_FILE}" ]]; then
    echo "ERROR: Firmware not found: ${FIRMWARE_FILE}" >&2
    exit 1
fi

echo "Firmware: ${FIRMWARE_FILE}"
ls -lh "${FIRMWARE_FILE}"

# Raw binaries carry no address information: write them at the flash base
# and sanity-check their size. ELF/HEX carry their own addresses.
DOWNLOAD_ARGS=(-d "${FIRMWARE_FILE}")
case "${FIRMWARE_FILE}" in
    *.bin)
        BIN_SIZE=$(stat -c %s "${FIRMWARE_FILE}")
        if (( BIN_SIZE > FLASH_SIZE_MAX )); then
            echo "ERROR: Firmware (${BIN_SIZE} bytes) is larger than the 256 KiB flash." >&2
            exit 1
        fi
        DOWNLOAD_ARGS=(-d "${FIRMWARE_FILE}" "${FLASH_ADDR}")
        ;;
esac

# Show the user which DFU devices are currently visible.
echo ""
echo "DFU devices detected:"
"${STM32_PROGRAMMER}" -l usb || true

echo ""
echo "Flashing ${FIRMWARE_FILE}..."
# -c port=usb1 : connect to the first STM32 ROM bootloader DFU device
# -d <file>    : download (ELF/HEX use their embedded addresses, .bin uses FLASH_ADDR)
# -v           : verify after download
# NOTE: '-rst' is NOT supported over DFU ("Reset command is only available
# with JTAG/SWD"), and the G4 ROM bootloader ignores a bare DFU detach.
# You must power-cycle the board or press its reset button afterwards.
"${STM32_PROGRAMMER}" -c port=usb1 "${DOWNLOAD_ARGS[@]}" -v

echo ""
echo "Done. Now reset the board (power-cycle or reset button, BOOT0 low)"
echo "so it boots the new firmware."
