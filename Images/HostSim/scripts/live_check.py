"""Live-mode realtime check: run host_sim live, drain telemetry, quit, report sim t vs wall."""
import socket
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
PORT = 14608
RUN_S = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
SCENARIO = sys.argv[2] if len(sys.argv) > 2 else "scenarios/ngspice_short.json"

proc = subprocess.Popen(
    ["./build/Debug/host_sim.exe", SCENARIO, "--live", "--realtime", "1.0"]
    + sys.argv[3:],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
)

# Drain the child's stdout/stderr pipe continuously: a full pipe blocks the
# child's prints and silently throttles the sim loop.
out_lines = []


def drain_pipe():
    for line in proc.stdout:
        out_lines.append(line)


drainer = threading.Thread(target=drain_pipe, daemon=True)
drainer.start()

start = time.monotonic()
# Wait for the listener to come up.
sock = None
while time.monotonic() - start < 5.0:
    try:
        sock = socket.create_connection((HOST, PORT), timeout=0.5)
        break
    except OSError:
        time.sleep(0.1)
if sock is None:
    print("FAIL: could not connect to telemetry port")
    proc.kill()
    sys.exit(1)

sock.setblocking(False)
drain_end = time.monotonic() + RUN_S
while time.monotonic() < drain_end:
    try:
        data = sock.recv(65536)
        if not data:
            break
    except BlockingIOError:
        time.sleep(0.005)
    except OSError:
        break

elapsed = time.monotonic() - start
alive_before_quit = proc.poll() is None
try:
    sock.sendall(b"quit\n")
except OSError:
    pass

try:
    proc.wait(timeout=5.0)
except subprocess.TimeoutExpired:
    proc.kill()
    print("FAIL: did not exit within 5 s of quit")
drainer.join(timeout=2.0)
out = "".join(out_lines)
print(f"alive_before_quit={alive_before_quit} exit_code={proc.returncode}")

sim_t = None
dbg = []
for line in out.splitlines():
    if "stopped at t=" in line:
        sim_t = float(line.split("stopped at t=")[1].split()[0])
    if "ngspice-dbg" in line or "simprof" in line:
        dbg.append(line.strip())
    for bad in ("stuck at", "did not reach", "ended early"):
        if bad in line:
            print("BAD LOG:", line.strip())

tail = [l for l in out.splitlines() if "HostSim live" in l]
print("\n".join(tail[-3:]))
for d in dbg[-6:]:
    print(d)
if sim_t is None:
    print("FAIL: no 'stopped at t=' line")
    sys.exit(1)
print(f"wall={elapsed:.3f}s sim={sim_t:.3f}s realtime_factor={sim_t/elapsed:.3f}")
