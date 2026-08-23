#!/usr/bin/env bash
#
# Flash the STM32G474 CoProcessor firmware over USB DFU.
#
# Target: STM32G474RCTx (256 KiB dual-bank Flash, from CoProcessor.ioc)
#   Bank 1: 0x0800 0000 - 0x0801 FFFF (128 KiB)
#   Gap:    0x0802 0000 - 0x0803 FFFF
#   Bank 2: 0x0804 0000 - 0x0805 FFFF (128 KiB)
#
# The ST ROM bootloader exposes the whole internal flash as DFU alternate
# setting 0 at the standard STM32 flash base address 0x0800 0000.  The DFU
# VID:PID of an STM32 in ROM bootloader mode is 0483:df11.
#
# References:
#   - ST AN2606 (system memory boot mode), STM32G47xxx/48xxx section
#   - ST AN3156 (USB DFU protocol used in STM32 bootloader)
#   - RM0440 Reference manual, embedded flash memory chapter
#
# Usage:
#   ./flash-dfu.sh [path/to/firmware.bin]
#
# If no firmware path is given, the script looks for:
#   build/Debug/CoProcessor.bin
# If only the ELF is present, it is converted to a raw binary with
# arm-none-eabi-objcopy before flashing.
#
# The board must be in STM32 ROM bootloader DFU mode before running this script.
# On many boards this is done by holding BOOT0 high while resetting, or by using
# a USB-PD / debug adapter that can request the bootloader.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_NAME="CoProcessor"
BUILD_PRESET="Debug"
FLASH_ADDR="0x08000000"
STM32_DFU_VID_PID="0483:df11"
STM32_DFU_INTF=0
STM32_DFU_ALT=0
# Total flash size for STM32G474RCTx (256 KiB = 262144 bytes)
FLASH_SIZE_MAX=$((256 * 1024))
# In dual-bank mode Bank 1 ends at 0x0801FFFF (128 KiB)
BANK1_SIZE=$((128 * 1024))

FIRMWARE_BIN="${1:-${SCRIPT_DIR}/build/${BUILD_PRESET}/${PROJECT_NAME}.bin}"

if ! command -v dfu-util &>/dev/null; then
    echo "ERROR: dfu-util is not installed or not in PATH." >&2
    echo "Install it (e.g. 'sudo apt install dfu-util') and try again." >&2
    exit 1
fi

# If a .bin wasn't provided / doesn't exist, try to generate one from the ELF.
if [[ ! -f "${FIRMWARE_BIN}" ]]; then
    FIRMWARE_ELF="${FIRMWARE_BIN%.bin}.elf"
    if [[ ! -f "${FIRMWARE_ELF}" ]]; then
        echo "Firmware not found: ${FIRMWARE_BIN}" >&2
        echo "Attempting CMake build (preset: ${BUILD_PRESET})..." >&2
        if ! command -v cmake &>/dev/null || ! command -v arm-none-eabi-gcc &>/dev/null; then
            echo "ERROR: CMake and/or arm-none-eabi-gcc are not available to build the firmware." >&2
            exit 1
        fi
        cmake --preset "${BUILD_PRESET}" -S "${SCRIPT_DIR}"
        cmake --build "${SCRIPT_DIR}/build/${BUILD_PRESET}"
    fi

    if [[ ! -f "${FIRMWARE_ELF}" ]]; then
        echo "ERROR: Build did not produce ${FIRMWARE_ELF}" >&2
        exit 1
    fi

    if ! command -v arm-none-eabi-objcopy &>/dev/null; then
        echo "ERROR: ${FIRMWARE_BIN} is missing and arm-none-eabi-objcopy is not available." >&2
        exit 1
    fi

    echo "Converting ${FIRMWARE_ELF} to ${FIRMWARE_BIN}..."
    arm-none-eabi-objcopy -O binary "${FIRMWARE_ELF}" "${FIRMWARE_BIN}"
fi

echo "Firmware: ${FIRMWARE_BIN}"
ls -lh "${FIRMWARE_BIN}"

BIN_SIZE=$(stat -c %s "${FIRMWARE_BIN}")
if (( BIN_SIZE > FLASH_SIZE_MAX )); then
    echo "ERROR: Firmware (${BIN_SIZE} bytes) is larger than the 256 KiB flash." >&2
    exit 1
fi

if (( BIN_SIZE > BANK1_SIZE )); then
    echo ""
    echo "WARNING: Firmware (${BIN_SIZE} bytes) exceeds Bank 1 (128 KiB)." >&2
    echo "         The STM32G474 dual-bank layout has a gap from 0x08020000 to 0x0803FFFF." >&2
    echo "         The ST ROM bootloader normally handles this mapping, but if the" >&2
    echo "         download fails, switch to single-bank mode or use STM32CubeProgrammer." >&2
fi

# Show the user which DFU devices are currently visible.
echo ""
echo "DFU devices detected:"
dfu-util --list || true

# STM32 ROM DFU: altsetting 0 is "Internal Flash" at 0x08000000.
# The ':leave' suffix tells dfu-util to issue the DFU Leave command so the
# bootloader jumps to the application after the download completes.
echo ""
echo "Flashing ${FIRMWARE_BIN} to ${FLASH_ADDR}..."
dfu-util \
    --device "${STM32_DFU_VID_PID}" \
    --intf "${STM32_DFU_INTF}" \
    --alt "${STM32_DFU_ALT}" \
    --download "${FIRMWARE_BIN}" \
    --dfuse-address "${FLASH_ADDR}:leave"

echo ""
echo "Done. The device should now reset and run the new firmware."
