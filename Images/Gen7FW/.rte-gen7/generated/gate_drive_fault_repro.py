#!/usr/bin/env python3
"""Gate-driver /FLT-after-reset repro loop.

Repeatedly pulses the NCD57100 RESET (via the `clearfault` shell command,
which also re-enables gate-driver power) and then samples /RDY and /FLT
~250 ms later via `status` -- long after the ~10 ms window where clearfault
itself reads the pins. On the affected module /FLT latches low 10-100 ms
after every reset release with no switching, so this loop shows
gd_fault=Y on every iteration without any PWM ever running.

Correlate with a scope on /FLT (and the DESAT pin): each iteration prints a
marker with its number and the reset happens right after it.

Usage: python3 gate_drive_fault_repro.py [iterations] [settle_s]
Defaults: 20 iterations, 0.25 s settle after the reset pulse.
"""

import glob
import re
import sys
import time

import serial

ITERATIONS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
# Sample times (seconds) after the reset release to read /RDY and /FLT.
SAMPLE_TIMES = [float(x) for x in (sys.argv[2].split(",") if len(sys.argv) > 2
                                   else "0.05,0.2,0.5,1.0".split(","))]


def find_bridge():
    ports = sorted(glob.glob(
        "/dev/serial/by-id/usb-OpenVVVF_Control_Board_Gen_7_*-if00"))
    if not ports:
        raise SystemExit("bridge port not found (board connected? RTE Studio closed?)")
    return ports[0]


def main():
    ser = serial.Serial()
    ser.port = find_bridge()
    ser.baudrate = 460800
    ser.parity = serial.PARITY_NONE
    ser.timeout = 0.25
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(1.0)
    ser.reset_input_buffer()

    def exchange(cmd, wait=1.5):
        ser.write(b"\r" + cmd.encode() + b"\r\n")
        ser.flush()
        data = b""
        t0 = time.monotonic()
        while time.monotonic() - t0 < wait:
            try:
                data += ser.read(4096)
            except serial.SerialException:
                break
        return data

    print(f"iterations={ITERATIONS} sample_times={SAMPLE_TIMES}s "
          f"(port {ser.port}) -- Ctrl-C to stop")
    t_prev = 0.0
    try:
        for i in range(1, ITERATIONS + 1):
            # Marker: the RESET pulse happens inside clearfault, right after this.
            print(f"=== iter {i}: reset pulse now ===", flush=True)
            exchange("clearfault", 1.2)

            for t in SAMPLE_TIMES:
                time.sleep(max(0.0, t - t_prev))
                t_prev = t
                resp = exchange("status", 1.0).decode("ascii", errors="replace")
                ready = re.search(r"ready=([YN])", resp)
                fault = re.search(r"gd_fault=([YN])", resp)
                print(f"    t=+{t:.2f}s  ready={ready.group(1) if ready else '?'} "
                      f"gd_fault={fault.group(1) if fault else '?'}")
            t_prev = 0.0
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("stopped by user")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
