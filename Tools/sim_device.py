#!/usr/bin/env python3
"""Simulated inverter device for testing NodeGUI's Runtime tab.

Creates a pty pair and speaks the real TLM1 telemetry protocol (COBS-framed,
CRC16-CCITT) on it, exactly like the firmware does:

  - MSG_DEFINE frames registering a set of float signals + the "print"
    console string (re-sent every few seconds so late clients catch up)
  - MSG_DATA frames at ~100 Hz with plausible sine/ramp/noise values
  - console lines via the "print" string signal
  - answers shell command lines with a "sim: got '<cmd>'" console reply

NodeGUI needs no changes — point it at the printed slave path:

    ./Tools/sim_device.py &
    ./build/bin/rte-studio --serial /dev/pts/N

or use --link to get a stable path:

    ./Tools/sim_device.py --link /tmp/fake_device &
    ./build/bin/rte-studio --serial /tmp/fake_device

Ctrl-C to stop. Requires Python 3.8+, stdlib only.
"""

import argparse
import math
import os
import random
import struct
import sys
import termios
import time
import tty

MAGIC = 0x544C4D31  # "TLM1"
VERSION = 1
MSG_DATA = 1
MSG_DEFINE = 2
VT_F32 = 1
VT_STR = 2

PRINT_ID = 0x8001

# name -> (id, frequency Hz, amplitude, offset)
SIGNALS = [
    ("vdc_v", 1, 0.2, 0.5, 24.0),
    ("ph_u_a", 2, 2.0, 8.0, 0.0),
    ("ph_v_a", 3, 2.0, 8.0, 0.0),
    ("ph_w_a", 4, 2.0, 8.0, 0.0),
    ("id_a", 5, 2.0, 3.0, 0.0),
    ("iq_a", 6, 0.5, 5.0, 2.0),
    ("V_bus", 7, 0.1, 0.2, 24.0),
    ("I_ROTOR_speed", 8, 0.3, 50.0, 400.0),
    ("enc_angle_deg", 9, 0.25, 180.0, 180.0),
    ("temp_c", 10, 0.05, 1.5, 35.0),
]


def crc16_ccitt(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


class FakeDevice:
    def __init__(self, rate_hz):
        self.rate_hz = rate_hz
        self.seq = 0
        self.t0 = time.monotonic()
        self.master, self.slave = os.openpty()
        self.slave_name = os.ttyname(self.slave)
        # Keep the slave fd open (closing it makes master reads fail with EIO
        # when no client is attached) and force raw mode: with no client
        # attached the pty's default ECHO would bounce our own frames back as
        # phantom "commands".
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.cmd_buf = bytearray()

    def time_us(self):
        return int((time.monotonic() - self.t0) * 1_000_000) & 0xFFFFFFFF

    def send_frame(self, msg_type, payload):
        self.seq += 1
        header = struct.pack("<IBBHII", MAGIC, VERSION, msg_type,
                             len(payload), self.seq, self.time_us())
        body = header + payload
        packet = body + struct.pack("<H", crc16_ccitt(body))
        try:
            os.write(self.master, cobs_encode(packet) + b"\x00")
        except BlockingIOError:
            pass  # pty buffer full (client not draining); drop, like a device

    def send_defines(self):
        entries = [(sid, VT_F32, name) for name, sid, *_ in SIGNALS]
        entries.append((PRINT_ID, VT_STR, "print"))
        payload = bytes([len(entries)])
        for sid, vtype, name in entries:
            key = name.encode()
            payload += struct.pack("<HBB", sid, vtype, len(key)) + key
        self.send_frame(MSG_DEFINE, payload)

    def send_data(self):
        t = time.monotonic() - self.t0
        payload = bytes([len(SIGNALS)])
        for i, (name, sid, freq, amp, offset) in enumerate(SIGNALS):
            value = offset + amp * math.sin(2 * math.pi * freq * t + i * 1.1)
            value += random.uniform(-0.05, 0.05)
            payload += struct.pack("<HB", sid, VT_F32) + struct.pack("<f", value)
        self.send_frame(MSG_DATA, payload)

    def send_print(self, text):
        data = text.encode()[:255]  # VT_STR length is one byte
        payload = bytes([1]) + struct.pack("<HB", PRINT_ID, VT_STR)
        payload += bytes([len(data)]) + data
        self.send_frame(MSG_DATA, payload)

    def poll_commands(self):
        try:
            chunk = os.read(self.master, 4096)
        except BlockingIOError:
            return
        if not chunk:
            return
        self.cmd_buf += chunk
        while b"\n" in self.cmd_buf:
            line, _, rest = self.cmd_buf.partition(b"\n")
            self.cmd_buf = bytearray(rest)
            cmd = line.decode(errors="replace").strip()
            if cmd:
                self.send_print(f"sim: got '{cmd}'")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rate", type=float, default=100.0,
                        help="telemetry rate in Hz (default 100)")
    parser.add_argument("--link", metavar="PATH",
                        help="symlink PATH to the pty for a stable device name")
    args = parser.parse_args()

    device = FakeDevice(args.rate)
    print(f"fake device on: {device.slave_name}", flush=True)
    if args.link:
        try:
            os.unlink(args.link)
        except FileNotFoundError:
            pass
        os.symlink(device.slave_name, args.link)
        print(f"linked at:      {args.link}", flush=True)
    print(f"connect with:   NodeGUI --serial {args.link or device.slave_name}",
          flush=True)

    device.send_defines()

    period = 1.0 / args.rate
    tick = 0
    next_tick = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now < next_tick:
                time.sleep(min(0.002, next_tick - now))
                device.poll_commands()
                continue

            device.send_data()
            tick += 1

            if tick % int(args.rate) == 0:
                device.send_print(f"sim: {tick // int(args.rate)} s uptime")
            if tick % int(args.rate * 5) == 0:
                device.send_defines()  # keep late/reconnected clients in sync

            device.poll_commands()
            next_tick += period
            if next_tick < now - 0.25:  # fell behind; don't burst
                next_tick = now
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
