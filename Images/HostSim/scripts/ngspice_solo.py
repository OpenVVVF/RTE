"""Isolate ngspice behavior: run a netlist via ngspice.dll with no-op callbacks.

Usage: python scripts/ngspice_solo.py <netlist.cir> [timeout_s]
External VSRCs are rewritten to plain DC sources so no sync init is needed.
"""
import ctypes
import re
import sys
import time

netlist_path = sys.argv[1]
timeout_s = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

ng = ctypes.CDLL(r"C:\Program Files\KiCad\10.0\bin\ngspice.dll")

SendChar = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
SendStat = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
ControlledExit = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_bool, ctypes.c_bool,
                                  ctypes.c_int, ctypes.c_void_p)
SendData = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                            ctypes.c_void_p)
SendInitData = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
BGThreadRunning = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_bool, ctypes.c_int, ctypes.c_void_p)

last_t = [0.0]


class VecValues(ctypes.Structure):
    pass


VecValues._fields_ = [
    ("name", ctypes.c_char_p),
    ("creal", ctypes.c_double),
    ("cimag", ctypes.c_double),
    ("is_scale", ctypes.c_int),
    ("is_complex", ctypes.c_int),
]


class VecValuesAll(ctypes.Structure):
    _fields_ = [("veccount", ctypes.c_int), ("vecindex", ctypes.c_int),
                ("vecsa", ctypes.POINTER(ctypes.POINTER(VecValues)))]


@SendChar
def send_char(output, ident, userdata):
    return 0


@SendStat
def send_stat(output, ident, userdata):
    return 0


@ControlledExit
def controlled_exit(status, immediate, quit_, ident, userdata):
    return 0


@SendData
def send_data(data, structcount, ident, userdata):
    if data:
        va = ctypes.cast(data, ctypes.POINTER(VecValuesAll)).contents
        for i in range(va.veccount):
            v = va.vecsa[i].contents
            if v.is_scale:
                last_t[0] = v.creal
                break
    return 0


@SendInitData
def send_init_data(data, ident, userdata):
    return 0


@BGThreadRunning
def bg_thread_running(running, ident, userdata):
    return 0


ng.ngSpice_Init(send_char, send_stat, controlled_exit, send_data, send_init_data,
                bg_thread_running, None)

with open(netlist_path) as f:
    lines = []
    for line in f.read().splitlines():
        if line.strip():
            lines.append(re.sub(r"\bexternal\b", "", line))
arr = (ctypes.c_char_p * (len(lines) + 1))()
for i, l in enumerate(lines):
    arr[i] = l.encode()
arr[len(lines)] = None

print("circ rc:", ng.ngSpice_Circ(arr))
print("bg_run rc:", ng.ngSpice_Command(b"bg_run"))

start = time.monotonic()
while ng.ngSpice_running() and time.monotonic() - start < timeout_s:
    time.sleep(0.05)

elapsed = time.monotonic() - start
if ng.ngSpice_running():
    ng.ngSpice_Command(b"bg_halt")
    time.sleep(0.5)
    print(f"HANG: still running after {elapsed:.1f}s, last_t={last_t[0]:.6g}")
else:
    print(f"DONE in {elapsed:.2f}s, last_t={last_t[0]:.6g}")
