#!/usr/bin/env python3
"""Alternate the induction V/Hz TargetHz variable over the RTE Studio API.

Connects to the active RTE Studio session and repeatedly sends:

    var set TargetHz 60
    var set TargetHz -60
    var set TargetHz 60
    ...

Usage:
    python3 Tools/alternate_target_hz.py
    python3 Tools/alternate_target_hz.py --interval 45 --duration 1200
    python3 Tools/alternate_target_hz.py --session /path/to/session.json
"""

import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path


def discover_session(path: Path) -> dict:
    with path.open() as f:
        desc = json.load(f)
    if desc.get("version") != 1 or desc.get("host") != "127.0.0.1":
        raise RuntimeError("invalid RTE Studio session descriptor")
    if not desc.get("token") or not isinstance(desc.get("port"), int):
        raise RuntimeError("session descriptor missing token or port")
    return desc


def send_command(host: str, port: int, token: str, command: str, timeout: float = 5.0) -> dict:
    request = json.dumps({"token": token, "method": "device.command",
                          "params": {"command": command}}).encode() + b"\n"

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect((host, port))
        s.sendall(request)

        response = b""
        while b"\n" not in response:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk

    if not response:
        raise RuntimeError("no response from RTE Studio")

    envelope = json.loads(response.decode().splitlines()[0])
    if not envelope.get("ok", False):
        raise RuntimeError(envelope.get("error", "RTE Studio rejected the request"))
    return envelope.get("result", {})


def default_session_path() -> Path:
    if xdg := os.environ.get("XDG_CACHE_HOME"):
        return Path(xdg) / "rte" / "sessions" / "current.json"
    return Path.home() / ".cache" / "rte" / "sessions" / "current.json"


def main():
    ap = argparse.ArgumentParser(description="Alternate TargetHz via RTE Studio")
    ap.add_argument("--session", type=Path, default=None,
                    help="Path to RTE Studio session descriptor")
    ap.add_argument("--interval", type=float, default=45.0,
                    help="Seconds between commands (default 45)")
    ap.add_argument("--duration", type=float, default=20 * 60,
                    help="Total run time in seconds (default 1200 = 20 min)")
    ap.add_argument("--values", nargs=2, type=float, default=[60.0, -60.0],
                    metavar=("VAL", "VAL"), help="Two values to alternate (default 60 -60)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print commands without sending them")
    args = ap.parse_args()

    session_path = args.session or default_session_path()
    print(f"[*] session: {session_path}")
    session = discover_session(session_path)

    host = session["host"]
    port = session["port"]
    token = session["token"]
    print(f"[*] RTE Studio at {host}:{port}")

    values = args.values
    start = time.time()
    end = start + args.duration
    index = 0

    while time.time() < end:
        value = values[index % len(values)]
        command = f"var set TargetHz {value:g}"
        elapsed = time.time() - start
        remaining = max(0.0, end - time.time())
        print(f"[{elapsed:6.1f}s, {remaining:6.1f}s left] {command}")

        if not args.dry_run:
            try:
                result = send_command(host, port, token, command)
                print(f"    -> {result}")
            except Exception as e:
                print(f"    -> ERROR: {e}", file=sys.stderr)

        index += 1

        # Sleep until next interval or until duration expires.
        next_time = start + index * args.interval
        sleep_s = next_time - time.time()
        if sleep_s > 0:
            time.sleep(sleep_s)

    print(f"[*] done after {time.time() - start:.1f}s")


if __name__ == "__main__":
    main()
