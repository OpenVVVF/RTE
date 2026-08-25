#!/usr/bin/env bash
set -euo pipefail
# Flash MainProcessor firmware through the CoProcessor bridge.
# Usage: ./flash-main.sh [SERIAL_PORT] [FIRMWARE_ELF]
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STM32_PROGRAMMER="${STM32_PROGRAMMER:-/home/tliao/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"
PORT="${1:-}"
FIRMWARE="${2:-}"

if [[ -z "${PORT}" ]]; then
    PORT="$(ls -1 /dev/serial/by-id/usb-OpenVVVF_* 2>/dev/null | head -n 1 || true)"
    if [[ -z "${PORT}" ]]; then
        echo "ERROR: CoProcessor VCP not found." >&2
        exit 1
    fi
fi

if [[ -z "${FIRMWARE}" ]]; then
    FIRMWARE="${PWD}/pleasework2.elf"
fi

if [[ ! -f "${FIRMWARE}" ]]; then
    echo "ERROR: Firmware not found: ${FIRMWARE}" >&2
    exit 1
fi

echo "==> Entering bootloader bridge on ${PORT}..."
python3 - "${PORT}" <<'PY'
import sys, serial, time
port = sys.argv[1]
with serial.Serial(port, 115200, timeout=8) as ser:
    ser.reset_input_buffer()
    ser.write(b"BOOTLOADER\r\n")
    ser.flush()
    deadline = time.time() + 8
    ok = False
    while time.time() < deadline:
        line = ser.readline()
        if line:
            text = line.decode('ascii', errors='replace').strip()
            print(f"    CoProcessor: {text}")
            if 'BOOTLOADER OK' in text:
                ok = True
                break
            if 'BOOTLOADER FAILED' in text:
                print("ERROR: Bootloader sync failed.", file=sys.stderr)
                sys.exit(1)
    if not ok:
        print("ERROR: Timeout waiting for BOOTLOADER OK.", file=sys.stderr)
        sys.exit(1)
PY

echo "==> Flashing MainProcessor: ${FIRMWARE}"
"${STM32_PROGRAMMER}" -c port="${PORT}" br=115200 P=EVEN db=8 sb=1 \
    -d "${FIRMWARE}" -v -g 0x08000000

echo "==> Exiting bridge mode..."
python3 - "${PORT}" <<'PY'
import sys, serial, time
port = sys.argv[1]
with serial.Serial(port, 115200, timeout=3) as ser:
    ser.reset_input_buffer()
    ser.write(b'\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE')
    ser.flush()
    deadline = time.time() + 3
    while time.time() < deadline:
        line = ser.readline()
        if line:
            text = line.decode('ascii', errors='replace').strip()
            print(f"    CoProcessor: {text}")
            if 'EXITAPP OK' in text:
                print("==> Bridge exited, main MCU running application.")
                sys.exit(0)
    print("WARNING: No EXITAPP OK received; bridge may still be active.", file=sys.stderr)
    sys.exit(1)
PY
