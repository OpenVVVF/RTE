#!/usr/bin/env python3
"""CAN session protocol test client (raw socketcan, no dependencies).

Exercises the Gen6FW CAN session layer end-to-end:
  HELLO -> ATTACH_RSP -> CAP_REQ(telem+cmd) -> CAP_RSP -> heartbeats ->
  telemetry frames -> shell command over CAN -> DETACH -> heartbeat-timeout
  re-attach.

Usage: python3 Tools/can_session_client.py [can0] [--id-base 0x700] [--timeout 12]
       Add --commands only when Can.Proto.AllowCmd=1 is intentionally enabled.
"""

import argparse
import socket
import struct
import sys
import time

IVP_MAGIC = 0x544C4D31
IVP_VERSION = 1
MSG_TELEMETRY_DATA, MSG_TELEMETRY_DEFINE = 1, 2
MSG_COMMAND_REQ, MSG_COMMAND_RSP = 3, 4
MSG_HELLO, MSG_ATTACH_RSP, MSG_CAP_REQ, MSG_CAP_RSP = 7, 8, 9, 10
MSG_DETACH, MSG_HEARTBEAT = 11, 12
CAP_TELEM, CAP_CMD = 0x01, 0x02
FLAG_START, FLAG_END = 0x80, 0x40


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def packet(msg_type, seq, payload=b""):
    hdr = struct.pack("<IBBHII", IVP_MAGIC, IVP_VERSION, msg_type, len(payload), seq, 0)
    body = hdr + payload
    return body + struct.pack("<H", crc16(body))


class CanTransport:
    """Segment/reassemble IVP packets over classic CAN frames."""

    def __init__(self, iface, id_base):
        self.s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self.s.bind((iface,))
        self.s.settimeout(0.05)
        self.id_tx = id_base          # host -> device
        self.id_rx = id_base + 1      # device -> host
        self.tx_seq = 0
        self.rx_buf = b""
        self.rx_expect = 0
        self.rx_seq = 0

    def _send_frame(self, can_id, data):
        d = bytes(data) + b"\x00" * (8 - len(data))
        self.s.send(struct.pack("=IB3x", can_id, len(data)) + d)

    def send_packet(self, pkt):
        hdr0 = FLAG_START | (self.tx_seq & 0x0F)
        self.tx_seq = (self.tx_seq + 1) & 0x0F
        total = struct.pack("<H", len(pkt))
        first = pkt[:5]
        self._send_frame(self.id_tx, bytes([hdr0]) + total + first)
        off = 5
        while off < len(pkt):
            chunk = pkt[off:off + 7]
            off += len(chunk)
            flags = self.tx_seq & 0x0F
            if off >= len(pkt):
                flags |= FLAG_END
            self.tx_seq = (self.tx_seq + 1) & 0x0F
            self._send_frame(self.id_tx, bytes([flags]) + chunk)

    def poll(self):
        """Return a complete reassembled packet, or None."""
        try:
            frame, _ = self.s.recvfrom(16)
        except socket.timeout:
            return None
        cid, dlc = struct.unpack("=IB3x", frame[:8])
        data = frame[8:8 + dlc]
        if cid != self.id_rx or dlc < 1:
            return None
        flags = data[0] & (FLAG_START | FLAG_END)
        seq = data[0] & 0x0F
        if flags & FLAG_START:
            self.rx_expect = struct.unpack("<H", data[1:3])[0]
            self.rx_buf = data[3:]
            self.rx_seq = (seq + 1) & 0x0F
        else:
            if self.rx_expect == 0 or seq != self.rx_seq:
                self.rx_expect = 0
                return None
            self.rx_buf += data[1:]
            self.rx_seq = (seq + 1) & 0x0F
        if (flags & FLAG_END) and self.rx_expect and len(self.rx_buf) >= self.rx_expect:
            pkt, self.rx_expect = self.rx_buf[:self.rx_expect], 0
            return pkt
        return None


def decode_packet(pkt):
    magic, ver, mtype, plen, seq, t = struct.unpack("<IBBHII", pkt[:16])
    if magic != IVP_MAGIC:
        return None
    if crc16(pkt[:-2]) != struct.unpack("<H", pkt[-2:])[0]:
        return None
    return mtype, pkt[16:16 + plen]


def command_req(req_id, line):
    args = bytes([1]) + bytes([5, len(line)]) + line.encode()  # arg_count, STR arg
    return bytes([0, req_id]) + args  # opcode=0, req_id, args


def parse_data_items(payload, telemetry_ids, max_show=6):
    n = payload[0]
    off = 1
    shown = []
    for _ in range(n):
        if off + 3 > len(payload):
            break
        iid, vt = struct.unpack("<HB", payload[off:off + 3]); off += 3
        if vt == 1:  # f32
            if off + 4 > len(payload):
                break
            val = struct.unpack("<f", payload[off:off + 4])[0]; off += 4
            if len(shown) < max_show:
                shown.append("%s=%.3g" % (telemetry_ids.get(iid, iid), val))
        elif vt == 2:  # complete string
            ln = payload[off]; off += 1 + ln
        else:        # string fragment
            ln = payload[off + 1]; off += 2 + ln
    return shown


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iface", nargs="?", default="can0")
    ap.add_argument("--id-base", type=lambda x: int(x, 0), default=0x700)
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--commands", action="store_true",
                    help="request and exercise the command capability")
    args = ap.parse_args()

    tp = CanTransport(args.iface, args.id_base)
    seq = 0
    telemetry_ids = {}
    data_count = 0

    print("[*] HELLO ->")
    tp.send_packet(packet(MSG_HELLO, seq)); seq += 1

    attached = False
    granted = 0
    cmd_done = False
    cmd_sent = False
    t_start = time.time()
    last_hb = time.time()

    while time.time() - t_start < args.timeout:
        if time.time() - last_hb > 1.0:
            tp.send_packet(packet(MSG_HEARTBEAT, seq)); seq += 1
            last_hb = time.time()

        pkt = tp.poll()
        if pkt is None:
            if attached and not granted:
                time.sleep(0.001)
            continue
        dec = decode_packet(pkt)
        if dec is None:
            print("[!] undecodable packet (%d bytes)" % len(pkt))
            continue
        mtype, payload = dec

        if mtype == MSG_ATTACH_RSP:
            allow = payload[0]
            name = payload[2:2 + payload[1]].decode(errors="replace")
            print("[<] ATTACH_RSP device=%s allow_mask=0x%02X" % (name, allow))
            attached = True
            req = bytes([CAP_TELEM | (CAP_CMD if args.commands else 0)])
            tp.send_packet(packet(MSG_CAP_REQ, seq, req)); seq += 1
            print("[*] CAP_REQ %s ->" % ("telem+cmd" if args.commands else "telem"))
        elif mtype == MSG_CAP_RSP:
            granted = payload[0]
            print("[<] CAP_RSP granted=0x%02X" % granted)
        elif mtype == MSG_TELEMETRY_DEFINE:
            n = payload[0]
            off = 1
            for _ in range(n):
                iid, vt = struct.unpack("<HB", payload[off:off + 3]); off += 3
                if vt == 1:
                    klen = payload[off]; off += 1
                    key = payload[off:off + klen].decode(errors="replace"); off += klen
                    telemetry_ids[iid] = key
                elif vt == 2:
                    klen = payload[off]; off += 1 + klen
                else:
                    klen = payload[off + 1]; off += 2 + klen
            print("[<] DEFINE %d ids (total known: %d)" % (n, len(telemetry_ids)))
        elif mtype == MSG_TELEMETRY_DATA:
            data_count += 1
            if data_count == 1 or data_count % 100 == 0:
                shown = parse_data_items(payload, telemetry_ids)
                print("[<] DATA #%d: %s" % (data_count, ", ".join(shown)))
        elif mtype == MSG_COMMAND_RSP:
            print("[<] COMMAND_RSP req_id=%d" % (payload[0] if payload else -1))
            cmd_done = True

        if args.commands and (granted & CAP_CMD) and not cmd_sent:
            line = b"var list"
            tp.send_packet(packet(MSG_COMMAND_REQ, seq, command_req(0x42, line.decode()))); seq += 1
            print("[*] COMMAND_REQ 'var list' ->")
            cmd_sent = True

    print("[*] DETACH ->")
    tp.send_packet(packet(MSG_DETACH, seq)); seq += 1
    print("[*] done (telemetry DATA frames seen: %d)" % data_count)


if __name__ == "__main__":
    main()
