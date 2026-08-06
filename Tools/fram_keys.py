#!/usr/bin/env python3
"""Read, save, load, and write FRAM-backed config keys over serial or HTTP.

The firmware already exposes the `config` shell command family for managing
the RteParamStore key/value store in FRAM.  This tool uses those commands to:

  * save / read   -- dump FRAM keys to a JSON file
  * load / write  -- restore FRAM keys from a JSON file
  * list          -- print current FRAM keys to stdout

Connection methods (pick one):
  * Direct serial: --port /dev/ttyACM0 [--baud 460800]
  * NodeGUI HTTP API: --http http://localhost:8080

Examples:
    python3 Tools/fram_keys.py --port /dev/ttyACM0 save my_config.json
    python3 Tools/fram_keys.py --port /dev/ttyACM0 load my_config.json
    python3 Tools/fram_keys.py --http http://localhost:8080 list
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from typing import Dict, List, Optional, Protocol, Tuple

DEFAULT_BAUD = 460800
DEFAULT_QUIET_TIMEOUT = 0.25
DEFAULT_READ_TIMEOUT = 2.0


class TransportError(Exception):
    pass


class DeviceInterface(Protocol):
    """Abstract interface for sending a shell command and receiving text output."""

    def send_command(self, line: str) -> List[str]:
        ...


class SerialInterface:
    """Talk to the firmware text shell through a pyserial port."""

    def __init__(self, port: str, baud: int = DEFAULT_BAUD):
        import serial

        self.port_name = port
        try:
            self.port = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.05,
            )
        except serial.SerialException as exc:
            raise TransportError(f"cannot open {port}: {exc}") from exc

        # Drain anything that was already in the buffer.
        self.port.reset_input_buffer()

    def send_command(self, line: str) -> List[str]:
        self.port.write((line + "\n").encode("utf-8", errors="replace"))
        self.port.flush()
        return self._read_response()

    def _read_response(self) -> List[str]:
        """Collect text lines until the line stream has been quiet for a while."""
        deadline = time.monotonic() + DEFAULT_READ_TIMEOUT
        quiet_deadline = time.monotonic() + DEFAULT_QUIET_TIMEOUT
        raw_buf = bytearray()
        lines: List[str] = []

        while time.monotonic() < quiet_deadline and time.monotonic() < deadline:
            chunk = self.port.read(4096)
            if chunk:
                raw_buf.extend(chunk)
                quiet_deadline = time.monotonic() + DEFAULT_QUIET_TIMEOUT
            else:
                time.sleep(0.01)

            # Extract complete lines as they arrive.
            while b"\n" in raw_buf:
                idx = raw_buf.index(b"\n")
                line_bytes = bytes(raw_buf[:idx])
                del raw_buf[: idx + 1]
                line = self._decode_line(line_bytes)
                if line:
                    lines.append(line)

        # Handle any trailing text that didn't end with a newline.
        if raw_buf:
            line = self._decode_line(bytes(raw_buf))
            if line:
                lines.append(line)

        return lines

    @staticmethod
    def _decode_line(data: bytes) -> str:
        # Strip carriage returns and trailing whitespace.
        data = data.replace(b"\r", b"")
        text = data.decode("utf-8", errors="replace").rstrip()
        return text

    def close(self) -> None:
        self.port.close()


class HttpInterface:
    """Talk to the running NodeGUI HTTP API (/api/command)."""

    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip("/")

    def send_command(self, line: str) -> List[str]:
        url = f"{self.base_url}/api/command?wait_ms=2000"
        body = json.dumps({"cmd": line}).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=10.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            raise TransportError(f"HTTP {exc.code}: {exc.read().decode('utf-8', errors='replace')}") from exc
        except Exception as exc:
            raise TransportError(f"HTTP request failed: {exc}") from exc

        lines: List[str] = []
        for entry in data.get("console", []):
            text = entry.get("text", "")
            for raw_line in text.splitlines():
                decoded = raw_line.replace("\r", "").rstrip()
                if decoded:
                    lines.append(decoded)
        return lines


class DryRunInterface:
    """No-op interface used for load --dry-run so no real device is needed."""

    def send_command(self, line: str) -> List[str]:
        return []


class FramKeysClient:
    """High-level client that reads/writes FRAM keys through shell commands."""

    # Lines from `config list` look like:
    #   [SHELL]   Hw.Temp.Board.Offset = 0.0040
    LIST_LINE_RE = re.compile(r"^\s*(?:\[SHELL\]\s+)?(\S+)\s*=\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*$")

    def __init__(self, interface: DeviceInterface):
        self.interface = interface

    def read_keys(self) -> Dict[str, float]:
        """Read all keys currently stored in FRAM."""
        lines = self.interface.send_command("config list")
        keys: Dict[str, float] = {}
        for line in lines:
            match = self.LIST_LINE_RE.match(line)
            if match:
                key, value_str = match.group(1), match.group(2)
                try:
                    keys[key] = float(value_str)
                except ValueError:
                    continue
        return keys

    def write_keys(self, keys: Dict[str, float], dry_run: bool = False) -> None:
        """Write key/value pairs to FRAM.

        For graph config nodes the firmware's `config set` only updates the live
        value; we follow it with `config save` so the value is persisted.  For
        raw KV-store keys `config save` fails harmlessly with "unknown config
        key", while `config set` already flushes to FRAM.
        """
        for key, value in keys.items():
            set_cmd = f"config set {key} {value:g}"
            save_cmd = f"config save {key}"
            if dry_run:
                print(f"[dry-run] {set_cmd}")
                print(f"[dry-run] {save_cmd}")
                continue
            self.interface.send_command(set_cmd)
            self.interface.send_command(save_cmd)

    def delete_all_keys(self, dry_run: bool = False) -> None:
        """Remove every key from FRAM."""
        cmd = "config deleteall"
        if dry_run:
            print(f"[dry-run] {cmd}")
            return
        self.interface.send_command(cmd)


def make_interface(args: argparse.Namespace) -> DeviceInterface:
    # load --dry-run does not need a real connection; everything else does.
    is_dry_run = getattr(args, "dry_run", False)
    if is_dry_run:
        return DryRunInterface()
    if args.http:
        return HttpInterface(args.http)
    if args.port:
        return SerialInterface(args.port, args.baud)
    raise TransportError("specify a connection: --port <serial> or --http <url> (or use load --dry-run)")


def cmd_save(client: FramKeysClient, args: argparse.Namespace) -> int:
    keys = client.read_keys()
    path = args.file
    with open(path, "w", encoding="utf-8") as f:
        json.dump(keys, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"Saved {len(keys)} key(s) to {path}")
    return 0


def cmd_load(client: FramKeysClient, args: argparse.Namespace) -> int:
    path = args.file
    with open(path, "r", encoding="utf-8") as f:
        keys = json.load(f)

    if not isinstance(keys, dict):
        print(f"Error: {path} must contain a JSON object {{\"key\": value, ...}}", file=sys.stderr)
        return 1

    parsed: Dict[str, float] = {}
    for key, value in keys.items():
        if not isinstance(key, str):
            print(f"Error: non-string key {key!r}", file=sys.stderr)
            return 1
        try:
            parsed[key] = float(value)
        except (TypeError, ValueError):
            print(f"Error: value for '{key}' is not numeric: {value!r}", file=sys.stderr)
            return 1

    if args.clear:
        client.delete_all_keys(dry_run=args.dry_run)

    client.write_keys(parsed, dry_run=args.dry_run)
    action = "Would load" if args.dry_run else "Loaded"
    print(f"{action} {len(parsed)} key(s) from {path}")
    return 0


def cmd_list(client: FramKeysClient, args: argparse.Namespace) -> int:
    keys = client.read_keys()
    for key in sorted(keys):
        print(f"{key} = {keys[key]:g}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Read/save/load/write FRAM-backed config keys via the firmware CLI.",
    )
    conn = parser.add_mutually_exclusive_group()
    conn.add_argument("--port", help="Serial port (e.g. /dev/ttyACM0 or COM3)")
    conn.add_argument("--http", help="NodeGUI HTTP API base URL (e.g. http://localhost:8080)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Serial baud rate (default {DEFAULT_BAUD})")

    sub = parser.add_subparsers(dest="command", required=True)

    save_p = sub.add_parser("save", aliases=["read"], help="Read FRAM keys and save them to a JSON file")
    save_p.add_argument("file", help="Output JSON file")

    load_p = sub.add_parser("load", aliases=["write"], help="Load keys from a JSON file and write them to FRAM")
    load_p.add_argument("file", help="Input JSON file")
    load_p.add_argument("--clear", action="store_true", help="Delete all FRAM keys before loading")
    load_p.add_argument("--dry-run", action="store_true", help="Print commands instead of sending them")

    list_p = sub.add_parser("list", help="Print current FRAM keys")

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    interface: Optional[DeviceInterface] = None
    try:
        interface = make_interface(args)
        client = FramKeysClient(interface)

        if args.command in ("save", "read"):
            return cmd_save(client, args)
        if args.command in ("load", "write"):
            return cmd_load(client, args)
        if args.command == "list":
            return cmd_list(client, args)

        parser.print_help()
        return 1
    except TransportError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if isinstance(interface, SerialInterface):
            interface.close()


if __name__ == "__main__":
    sys.exit(main())
