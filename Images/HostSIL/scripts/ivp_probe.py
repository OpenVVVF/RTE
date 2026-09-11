#!/usr/bin/env python3
"""ivp_probe.py — decode the COBS-framed InverterProtocol stream host_sil
serves in --live mode (or any TCP endpoint carrying the firmware's UART
telemetry stream) and print the firmware-emitted keys with live values.

Usage:
    python3 scripts/ivp_probe.py [host:port] [--seconds N] [--quiet-data]

This is a debug/verification tool: RTEStudio (--tcp H:P --protocol ivp) is
the real consumer.  Stdlib only.
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
VT_F32 = 1
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
    """Decode one COBS frame (without the trailing 0x00). None on error."""
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
    """Return (msg_type, seq, time_us, payload) or None if invalid."""
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("endpoint", nargs="?", default="127.0.0.1:14608")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--quiet-data", action="store_true",
                    help="print defines and a final summary only")
    args = ap.parse_args()
    host, _, port = args.endpoint.partition(":")
    port = int(port or 14608)

    sock = socket.create_connection((host, port), timeout=3.0)
    sock.settimeout(0.2)
    print(f"[probe] connected to {host}:{port}")

    keys = {}            # id -> key
    last_vals = {}       # key -> (value, time_us)
    good = bad = 0
    rx = bytearray()
    t_deadline = time.monotonic() + args.seconds

    while time.monotonic() < t_deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            print("[probe] server closed connection")
            break
        rx += chunk
        while True:
            try:
                delim = rx.index(0)
            except ValueError:
                break
            frame, _, rest = bytes(rx[:delim]), None, rx[delim + 1:]
            rx = bytearray(rest)
            decoded = cobs_decode(frame)
            parsed = parse_frame(decoded) if decoded else None
            if parsed is None:
                bad += 1 if len(frame) > 4 else 0   # tolerate partial first frame
                continue
            msg_type, seq, time_us, payload = parsed
            good += 1
            if msg_type == MSG_DEFINE:
                n = payload[0]
                p = 1
                for _ in range(n):
                    sid, vtype, klen = struct.unpack_from("<HBB", payload, p)
                    p += 4
                    key = payload[p:p + klen].decode("ascii", "replace")
                    p += klen
                    keys[sid] = key
                    print(f"[probe] define id={sid:#06x} type={vtype} key={key!r}")
            elif msg_type == MSG_DATA:
                n = payload[0]
                p = 1
                for _ in range(n):
                    sid, vtype = struct.unpack_from("<HB", payload, p)
                    p += 3
                    key = keys.get(sid, f"id={sid:#06x}")
                    if vtype == VT_F32:
                        (val,) = struct.unpack_from("<f", payload, p)
                        p += 4
                        last_vals[key] = (val, time_us)
                        if not args.quiet_data:
                            print(f"[probe] data t={time_us}us {key} = {val:.6g}")
                    elif vtype == VT_STR:
                        slen = payload[p]
                        p += 1
                        s = payload[p:p + slen].decode("utf-8", "replace")
                        p += slen
                        print(f"[probe] str t={time_us}us {key} = {s!r}")
                    elif vtype == VT_STR_FRAG:
                        frag, slen = payload[p], payload[p + 1]
                        p += 2
                        s = payload[p:p + slen].decode("utf-8", "replace")
                        p += slen
                        print(f"[probe] strfrag t={time_us}us {key} flags={frag:#x} = {s!r}")
                    else:
                        print(f"[probe] data item id={sid:#06x} unknown vtype={vtype}")

    print(f"[probe] frames: good={good} bad={bad}")
    print(f"[probe] keys defined: {sorted(keys.values())}")
    if last_vals:
        print("[probe] last f32 values:")
        for k in sorted(last_vals):
            val, t = last_vals[k]
            print(f"[probe]   {k:24s} = {val:.6g}   (t={t}us)")
    f32_seen = len(last_vals)
    print(f"[probe] f32 keys with data: {f32_seen}")
    return 0 if good > 0 and f32_seen >= 1 else 1


if __name__ == "__main__":
    sys.exit(main())
