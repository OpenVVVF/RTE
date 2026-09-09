#!/usr/bin/env python3
"""shell_client.py — send Gen6FW CommandShell text commands over the host_sil
--live TCP link (or hardware) and print the shell's responses, which arrive
as 'print' string values inside the COBS-framed InverterProtocol telemetry
stream (same decoder core as ivp_probe.py).

Usage:
    python3 scripts/shell_client.py [host:port] [options] COMMAND [COMMAND ...]

Each COMMAND is sent verbatim with a trailing '\\n' appended.  Options:
    --lead-in-s N   seconds to listen before sending the first command
                    (default 0.5 — lets DEFINE frames map the 'print' key)
    --settle-s N    seconds to listen after the last command (default 2.0)

Stdlib only.  Example:
    python3 scripts/shell_client.py 127.0.0.1:14608 \
        "help" "var get IqVar" "var set IqVar 5.0" "var get IqVar"
"""
import argparse
import socket
import struct
import sys
import time

IVP_MAGIC = 0x544C4D31          # "TLM1"
IVP_VERSION = 1
MSG_DATA = 1
MSG_DEFINE = 2
VT_STR = 2
VT_STR_FRAG = 3


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_decode(frame: bytes):
    if not frame:
        return None
    out = bytearray()
    i = 0
    while i < len(frame):
        code = frame[i]
        if code == 0 or i + code > len(frame) + 1:
            return None
        i += 1
        out += frame[i:i + code - 1]
        i += code - 1
        if code < 0xFF and i < len(frame):
            out.append(0)
    return bytes(out)


def parse_frame(raw: bytes):
    if len(raw) < 16 + 2:
        return None
    magic, version, msg_type, payload_len, seq, time_us = struct.unpack_from("<IBBHII", raw, 0)
    if magic != IVP_MAGIC or version != IVP_VERSION:
        return None
    if len(raw) != 16 + payload_len + 2:
        return None
    crc = struct.unpack_from("<H", raw, 16 + payload_len)[0]
    if crc != crc16_ccitt(raw[:16 + payload_len]):
        return None
    return msg_type, seq, time_us, raw[16:16 + payload_len]


class StreamDecoder:
    """Accumulate TCP bytes, decode frames, expose shell 'print' strings."""

    def __init__(self):
        self.rx = bytearray()
        self.keys = {}          # id -> key
        self.prints = []        # [str] shell print strings, in arrival order
        self.good = 0

    def feed(self, chunk: bytes):
        self.rx += chunk
        while True:
            try:
                delim = self.rx.index(0)
            except ValueError:
                break
            frame, rest = bytes(self.rx[:delim]), self.rx[delim + 1:]
            self.rx = bytearray(rest)
            decoded = cobs_decode(frame)
            parsed = parse_frame(decoded) if decoded else None
            if parsed is None:
                continue
            self.good += 1
            msg_type, _seq, time_us, payload = parsed
            if msg_type == MSG_DEFINE:
                n = payload[0]
                p = 1
                for _ in range(n):
                    sid, vtype, klen = struct.unpack_from("<HBB", payload, p)
                    p += 4
                    key = payload[p:p + klen].decode("ascii", "replace")
                    p += klen
                    self.keys[sid] = key
            elif msg_type == MSG_DATA:
                n = payload[0]
                p = 1
                for _ in range(n):
                    sid, vtype = struct.unpack_from("<HB", payload, p)
                    p += 3
                    key = self.keys.get(sid, f"id={sid:#06x}")
                    if vtype == VT_STR:
                        slen = payload[p]
                        p += 1
                        s = payload[p:p + slen].decode("utf-8", "replace")
                        p += slen
                        print(f"[shell {time_us / 1e6:8.3f}s] {key}: {s}")
                        if key == "print":
                            self.prints.append(s)
                    elif vtype == VT_STR_FRAG:
                        frag, slen = payload[p], payload[p + 1]
                        p += 2
                        s = payload[p:p + slen].decode("utf-8", "replace")
                        p += slen
                        print(f"[shell {time_us / 1e6:8.3f}s] {key} frag {frag:#x}: {s}")
                    elif vtype == 1:
                        p += 4


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("endpoint", nargs="?", default="127.0.0.1:14608")
    ap.add_argument("--lead-in-s", type=float, default=0.5)
    ap.add_argument("--settle-s", type=float, default=2.0)
    ap.add_argument("commands", nargs=argparse.REMAINDER,
                    help="shell command lines; options must come before them")
    args = ap.parse_args()
    if not args.commands:
        ap.error("at least one COMMAND is required")

    host, _, port = args.endpoint.partition(":")
    port = int(port or 14608)

    sock = socket.create_connection((host, port), timeout=3.0)
    sock.settimeout(0.1)
    print(f"[client] connected to {host}:{port}")

    dec = StreamDecoder()

    def listen_for(seconds: float):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                print("[client] server closed connection")
                return False
            dec.feed(chunk)
        return True

    if not listen_for(args.lead_in_s):
        return 1

    for cmd in args.commands:
        print(f"[client] sending: {cmd!r}")
        sock.sendall(cmd.encode("utf-8") + b"\n")
        if not listen_for(args.settle_s):
            return 1

    print(f"[client] frames decoded: {dec.good}; print strings: {len(dec.prints)}")
    return 0 if dec.good > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
