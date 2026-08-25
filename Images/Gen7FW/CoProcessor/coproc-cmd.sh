#!/usr/bin/env bash
set -euo pipefail

# Send a command to the CoProcessor and read the response.
# Usage: ./coproc-cmd.sh [COMMAND] [SERIAL_PORT]

CMD="${1:-STATUS}"
PORT="${2:-}"

if [[ -z "${PORT}" ]]; then
    PORT="$(find /dev/serial/by-id -maxdepth 1 -type l -name 'usb-OpenVVVF_*if02*' -print 2>/dev/null | sort | head -n 1 || true)"
    if [[ -z "${PORT}" ]]; then
        for tty_path in /sys/class/tty/ttyACM*; do
            [[ -e "${tty_path}" ]] || continue
            if [[ "$(udevadm info --query=property --path="${tty_path}" 2>/dev/null | sed -n 's/^ID_USB_INTERFACE_NUM=//p')" == "02" ]]; then
                PORT="/dev/${tty_path##*/}"
                break
            fi
        done
    fi
    if [[ -z "${PORT}" ]]; then
        echo "ERROR: CoProcessor VCP not found." >&2
        exit 1
    fi
fi

echo "==> Sending ${CMD} to ${PORT}..."
python3 - "${PORT}" "${CMD}" <<'PY'
import sys, serial, time
port = sys.argv[1]
cmd = sys.argv[2].encode() + b"\r\n"

with serial.Serial(port, 115200, timeout=3) as ser:
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()
    deadline = time.time() + 3
    while time.time() < deadline:
        data = ser.read(ser.in_waiting or 1)
        if data:
            print(data.decode('ascii', errors='replace'), end='', flush=True)
    print()
PY
