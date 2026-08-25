#!/usr/bin/env bash
set -euo pipefail
# Flash through the CoProcessor's dual CDC device.
# Usage: ./flash-main.sh [BRIDGE_PORT] [CONTROL_PORT] [FIRMWARE]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STM32_PROGRAMMER="${STM32_PROGRAMMER:-/home/tliao/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI}"
FLASH_ATTEMPTS="${FLASH_ATTEMPTS:-3}"
BRIDGE_PORT="${1:-}"
CONTROL_PORT="${2:-}"
FIRMWARE="${3:-${SCRIPT_DIR}/pleasework2.elf}"

find_port() {
    local interface="$1"
    local port
    port="$(find /dev/serial/by-id -maxdepth 1 -type l -name "usb-OpenVVVF_*${interface}*" -print 2>/dev/null | sort | head -n 1 || true)"
    if [[ -z "${port}" ]]; then
        for tty_path in /sys/class/tty/ttyACM*; do
            [[ -e "${tty_path}" ]] || continue
            if [[ "$(udevadm info --query=property --path="${tty_path}" 2>/dev/null | sed -n 's/^ID_USB_INTERFACE_NUM=//p')" == "${interface#if}" ]]; then
                port="/dev/${tty_path##*/}"
                break
            fi
        done
    fi
    printf '%s' "${port}"
}

[[ -n "${BRIDGE_PORT}" ]] || BRIDGE_PORT="$(find_port if00)"
[[ -n "${CONTROL_PORT}" ]] || CONTROL_PORT="$(find_port if02)"
if [[ -z "${BRIDGE_PORT}" || -z "${CONTROL_PORT}" ]]; then
    echo "ERROR: Dual-CDC CoProcessor ports not found (bridge=if00, control=if02)." >&2
    exit 1
fi
[[ -f "${FIRMWARE}" ]] || { echo "ERROR: Firmware not found: ${FIRMWARE}" >&2; exit 1; }

control_command() {
    python3 - "$CONTROL_PORT" "$1" <<'PY'
import serial, sys, time
port, command = sys.argv[1:]
with serial.Serial(port, 115200, timeout=0.25) as ser:
    ser.reset_input_buffer()
    ser.write(command.encode("ascii") + b"\r\n")
    ser.flush()
    deadline = time.monotonic() + 3
    response = b""
    while time.monotonic() < deadline:
        response += ser.read(ser.in_waiting or 1)
        if b" OK\r\n" in response:
            print(response.decode("ascii", errors="replace"), end="")
            raise SystemExit(0)
    print(response.decode("ascii", errors="replace"), end="", file=sys.stderr)
    raise SystemExit(f"timeout waiting for {command} OK")
PY
}

restore_app() {
    local status=$?
    trap - EXIT INT TERM
    echo "==> Restoring main MCU application mode..."
    control_command APP || true
    exit "$status"
}
trap restore_app EXIT INT TERM

echo "==> Bridge:  ${BRIDGE_PORT}"
echo "==> Control: ${CONTROL_PORT}"
echo "==> Entering main MCU ROM bootloader..."
control_command BOOTLOADER
flash_ok=0
for ((attempt=1; attempt<=FLASH_ATTEMPTS; attempt++)); do
    echo "==> Flashing ${FIRMWARE} (attempt ${attempt}/${FLASH_ATTEMPTS})..."
    if "${STM32_PROGRAMMER}" -c port="${BRIDGE_PORT}" br=115200 P=EVEN db=8 sb=1 -d "${FIRMWARE}" -v -g 0x08000000; then
        flash_ok=1
        break
    fi
    if (( attempt < FLASH_ATTEMPTS )); then
        echo "WARNING: Flash attempt failed; resetting the ROM bootloader before retry." >&2
        control_command BOOTLOADER
    fi
done
if (( flash_ok == 0 )); then
    echo "ERROR: Flashing failed after ${FLASH_ATTEMPTS} attempts." >&2
    exit 1
fi
echo "==> Flash verified."
