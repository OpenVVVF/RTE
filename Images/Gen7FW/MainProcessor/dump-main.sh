#!/usr/bin/env bash
set -euo pipefail

# Dump the MainProcessor flash image over UART through the CoProcessor bridge.
# The CoProcessor enters BRIDGE mode, which drives the Main MCU BOOT0/NRST
# lines and then transparently forwards bytes to the STM32H7 ROM bootloader.
#
# Usage:
#   ./dump-main.sh [SERIAL_PORT] [OUTPUT_FILE]
#
# Defaults:
#   SERIAL_PORT -> auto-detected CoProcessor VCP, or /dev/ttyACM0
#   OUTPUT_FILE -> backup-main-YYYYMMDD-HHMMSS.bin

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STM32_PROGRAMMER="${STM32_PROGRAMMER:-/home/tliao/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"

PORT="${1:-}"
OUTPUT_FILE="${2:-}"

# STM32H723ZGTx has 1 MiB internal Flash starting at 0x08000000
FLASH_ADDR="0x08000000"
FLASH_SIZE="0x100000"

# Default output file with timestamp
if [[ -z "${OUTPUT_FILE}" ]]; then
    OUTPUT_FILE="${SCRIPT_DIR}/backup-main-$(date +%Y%m%d-%H%M%S).bin"
fi

# --- Serial port auto-detection -----------------------------------------------

if [[ -z "${PORT}" ]]; then
    if [[ -d /dev/serial/by-id ]]; then
        mapfile -t CANDIDATES < <(ls -1 /dev/serial/by-id/usb-OpenVVVF_* 2>/dev/null || true)
        if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
            PORT="${CANDIDATES[0]}"
            echo "==> Auto-detected CoProcessor VCP: ${PORT}"
        elif [[ ${#CANDIDATES[@]} -gt 1 ]]; then
            echo "ERROR: Multiple OpenVVVF VCPs found:" >&2
            printf '  %s\n' "${CANDIDATES[@]}" >&2
            echo "Specify one explicitly as the first argument." >&2
            exit 1
        fi
    fi

    if [[ -z "${PORT}" ]]; then
        PORT="/dev/ttyACM0"
        echo "==> Falling back to default VCP: ${PORT}"
    fi
fi

if [[ ! -e "${PORT}" ]]; then
    echo "ERROR: Serial port does not exist: ${PORT}" >&2
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required for the bootloader handshake." >&2
    exit 1
fi

python3 - <<'PY'
import sys
import importlib.util
if importlib.util.find_spec("serial") is None:
    print("ERROR: Python 'pyserial' module is not installed.", file=sys.stderr)
    print("Install it with: python3 -m pip install pyserial", file=sys.stderr)
    raise SystemExit(1)
PY

# --- Put CoProcessor into bootloader bridge mode ------------------------------

echo "==> Entering bootloader bridge mode on ${PORT}..."

python3 - "${PORT}" <<'PY'
import sys
import serial
import time

port = sys.argv[1]

try:
    ser = serial.Serial(port, baudrate=115200, bytesize=8, parity='N', stopbits=1, timeout=0.1)
except Exception as e:
    print(f"ERROR: Could not open {port}: {e}", file=sys.stderr)
    sys.exit(1)

with ser:
    ser.write(b'BRIDGE\r\n')
    ser.flush()

    deadline = time.time() + 8.0
    ok = False
    while time.time() < deadline:
        try:
            line = ser.readline()
        except serial.SerialException:
            break
        if line:
            text = line.decode('ascii', errors='replace').strip()
            print(f"    CoProcessor: {text}")
            if 'BRIDGE OK' in text:
                ok = True
                break
            if 'BOOTLOADER FAILED' in text:
                print("ERROR: Coprocessor could not enter bootloader mode.", file=sys.stderr)
                sys.exit(1)

    if not ok:
        print("ERROR: Timeout waiting for BRIDGE OK from coprocessor.", file=sys.stderr)
        sys.exit(1)

    # Drain any trailing debug text so the programmer does not see stale bytes.
    time.sleep(0.2)
    ser.reset_input_buffer()
PY

echo "==> Bootloader bridge active."

# --- Dump flash via STM32CubeProgrammer CLI -----------------------------------

# Note: parity must be EVEN for the STM32 ROM bootloader UART interface.
echo "==> Reading ${FLASH_SIZE} bytes from ${FLASH_ADDR} into ${OUTPUT_FILE}..."
"${STM32_PROGRAMMER}" -c port="${PORT}" br=115200 P=EVEN db=8 sb=1 \
    -u "${FLASH_ADDR}" "${FLASH_SIZE}" "${OUTPUT_FILE}"

echo "==> Dump complete: ${OUTPUT_FILE}"
ls -lh "${OUTPUT_FILE}"

echo ""
echo "NOTE: The CoProcessor is still in bootloader bridge mode."
echo "      Reset the CoProcessor (power cycle or reset button) to return it to normal operation."
