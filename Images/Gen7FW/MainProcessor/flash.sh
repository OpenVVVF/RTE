#!/usr/bin/env bash
set -euo pipefail

# Flash the MainProcessor firmware through the CoProcessor's USB-CDC bootloader bridge.
#
# Usage:
#   ./flash.sh [SERIAL_PORT] [FIRMWARE_FILE]
#
# Defaults:
#   SERIAL_PORT   -> auto-detected CoProcessor VCP, or /dev/ttyACM0
#   FIRMWARE_FILE -> build/Debug/MainProcessor.hex

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

print_usage() {
    cat <<EOF
Usage: $0 [SERIAL_PORT] [FIRMWARE_FILE]

  SERIAL_PORT    Serial device of the CoProcessor VCP (default: auto-detect)
  FIRMWARE_FILE  .hex or .bin firmware image (default: build/Debug/MainProcessor.hex)

Examples:
  $0
  $0 /dev/ttyACM1
  $0 /dev/ttyACM0 build/Release/MainProcessor.hex
EOF
}

# --- Argument parsing ---------------------------------------------------------

PORT=""
HEX=""

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            print_usage
            exit 0
            ;;
    esac
done

if [[ $# -ge 1 ]]; then
    PORT="$1"
fi
if [[ $# -ge 2 ]]; then
    HEX="$2"
fi

# --- Default firmware ---------------------------------------------------------

if [[ -z "$HEX" ]]; then
    HEX="${PROJECT_DIR}/build/Debug/MainProcessor.hex"
fi

if [[ ! -f "$HEX" ]]; then
    echo "ERROR: Firmware file not found: $HEX" >&2
    echo "Build the MainProcessor first with: ./build.sh" >&2
    exit 1
fi

# --- Serial port auto-detection -----------------------------------------------

if [[ -z "$PORT" ]]; then
    # The CoProcessor USB descriptor uses:
    #   VID=0xCAFE, PID=0x0001
    #   Manufacturer="OpenVVVF", Product="Control Board Gen 7"
    # Linux exposes this under /dev/serial/by-id with a predictable symlink.
    if [[ -d /dev/serial/by-id ]]; then
        mapfile -t CANDIDATES < <(ls -1 /dev/serial/by-id/usb-OpenVVVF_* 2>/dev/null || true)
        if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
            PORT="${CANDIDATES[0]}"
            echo "==> Auto-detected CoProcessor VCP: $PORT"
        elif [[ ${#CANDIDATES[@]} -gt 1 ]]; then
            echo "ERROR: Multiple OpenVVVF VCPs found:" >&2
            printf '  %s\n' "${CANDIDATES[@]}" >&2
            echo "Specify one explicitly as the first argument." >&2
            exit 1
        fi
    fi

    if [[ -z "$PORT" ]]; then
        PORT="/dev/ttyACM0"
        echo "==> Falling back to default VCP: $PORT"
    fi
fi

if [[ ! -e "$PORT" ]]; then
    echo "ERROR: Serial port does not exist: $PORT" >&2
    exit 1
fi

# --- Tool checks --------------------------------------------------------------

if ! command -v stm32flash >/dev/null 2>&1; then
    echo "ERROR: stm32flash is not installed." >&2
    echo "Install it with your package manager, e.g.:" >&2
    echo "  sudo apt install stm32flash" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
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

# --- Bootloader handshake -----------------------------------------------------

echo "==> Entering bootloader bridge mode on ${PORT}..."

python3 - "$PORT" <<'PY'
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
    # Use BRIDGE mode: the CoProcessor only drives BOOT0/NRST and then
    # transparently forwards bytes. stm32flash will perform the 0x7F sync itself.
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

    # Drain any trailing debug text so stm32flash does not see stale bytes.
    time.sleep(0.2)
    ser.reset_input_buffer()
PY

echo "==> Bootloader bridge active."

# --- Flash --------------------------------------------------------------------

# Run stm32flash without -c. The CoProcessor has reset the H7 into bootloader
# mode; stm32flash performs the normal 0x7F -> 0x79 init itself.
echo "==> Flashing ${HEX}..."
stm32flash -b 115200 -w "$HEX" -v -g 0x08000000 "$PORT"

echo "==> Flash complete."
echo ""
echo "NOTE: The CoProcessor is still in bootloader bridge mode."
echo "      Reset the CoProcessor (power cycle or reset button) to return it to normal operation."
