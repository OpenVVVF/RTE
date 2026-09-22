#!/usr/bin/env python3
"""Exercise MCP discovery and Gen7 flash control without attached hardware."""

import json
import binascii
import os
import pathlib
import pty
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time


def request(server, ident, method, params=None):
    message = {"jsonrpc": "2.0", "id": ident, "method": method}
    if params is not None:
        message["params"] = params
    server.stdin.write(json.dumps(message) + "\n")
    server.stdin.flush()
    answer = json.loads(server.stdout.readline())
    assert answer["id"] == ident, answer
    assert "error" not in answer, answer
    return answer["result"]


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    workspace = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        by_id = root / "by-id"
        by_id.mkdir()
        bridge_master, bridge_slave = pty.openpty()
        control_master, control_slave = pty.openpty()
        bridge = by_id / "usb-OpenVVVF_Test-if00"
        control = by_id / "usb-OpenVVVF_Test-if02"
        bridge.symlink_to(os.ttyname(bridge_slave))
        control.symlink_to(os.ttyname(control_slave))
        received = []
        bridge_mode = ["APP_8N1"]
        stop = threading.Event()

        def control_worker():
            pending = b""
            while not stop.is_set():
                try:
                    chunk = os.read(control_master, 256)
                except OSError:
                    continue
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    command = line.strip().decode("ascii")
                    received.append(command)
                    if command == "STATUS":
                        response = ("STATUS BOOTSEL=0 RESET=1 UART_MODE="
                                    + bridge_mode[0]
                                    + " UART_ERRORS=0 RX_DROPPED=0 TX_DROPPED=0\r\n")
                    else:
                        if command == "BOOTLOADER":
                            bridge_mode[0] = "BOOT_8E1"
                        elif command == "APP":
                            bridge_mode[0] = "APP_8N1"
                        response = command + " OK\r\n"
                    os.write(control_master, response.encode("ascii"))

        worker = threading.Thread(target=control_worker, daemon=True)
        worker.start()
        firmware = root / "main.bin"
        firmware.write_bytes((0x20010000).to_bytes(4, "little")
                             + (0x08001235).to_bytes(4, "little") + b"\xff" * 8)
        failing_firmware = root / "fail.bin"
        failing_firmware.write_bytes(firmware.read_bytes())
        log = root / "programmer.jsonl"
        programmer = root / "STM32_Programmer_CLI"
        programmer.write_text("#!/usr/bin/env python3\n"
                              "import json, os, sys\n"
                              "with open(os.environ['RTE_TEST_PROGRAMMER_LOG'], 'a') as f:\n"
                              "    f.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                              "print('verified 100%')\n"
                              "sys.exit(1 if any('fail.bin' in arg for arg in sys.argv) else 0)\n")
        programmer.chmod(0o755)
        env = dict(os.environ, RTE_SERIAL_BY_ID_DIR=str(by_id),
                   RTE_TEST_PROGRAMMER_LOG=str(log))

        class SessionHandler(socketserver.StreamRequestHandler):
            def handle(self):
                message = json.loads(self.rfile.readline())
                assert message["token"] == "test-token"
                method = message["method"]
                if method == "device.telemetry":
                    self.server.good_frames += 10
                    value = {"signals": {"phase_current_a": 1.25, "dc_bus_v": 48.0},
                             "strings": {"state": "idle"}, "rx_hz": 200.0,
                             "good_frames": self.server.good_frames}
                elif method == "device.status":
                    value = {"connected": True, "device_port": str(bridge),
                             "transport": self.server.transport, "suspended": False}
                elif method in ("device.history", "device.string_history"):
                    value = {"signal": message["params"]["signal"],
                             "samples": [{"time_s": 0.1,
                                          "value": "idle" if method == "device.string_history" else 1.25}]}
                elif method == "device.command":
                    self.server.commands.append(message["params"]["command"])
                    value = {"sent": True, "command": self.server.commands[-1],
                             "console_since": 4}
                elif method == "device.console":
                    value = {"lines": [{"seq": 5, "text": "OK: " + self.server.commands[-1]}]
                             if self.server.commands else [], "latest_seq": 5}
                else:
                    value = {"connected": True, "device_port": "mock"}
                self.wfile.write((json.dumps({"ok": True, "result": value}) + "\n").encode())

        session_server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), SessionHandler)
        session_server.daemon_threads = True
        session_server.commands = []
        session_server.good_frames = 100
        session_server.transport = "serial"
        session_worker = threading.Thread(target=session_server.serve_forever, daemon=True)
        session_worker.start()
        descriptor = root / "session.json"
        descriptor.write_text(json.dumps({"version": 1, "app": "RTE Studio",
                         "host": "127.0.0.1", "port": session_server.server_address[1],
                         "token": "test-token", "pid": 1}))
        server = subprocess.Popen([str(executable), "mcp", "--workspace", str(workspace),
                                   "--session", str(descriptor)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env)
        try:
            info = request(server, 1, "initialize", {"protocolVersion": "2025-03-26",
                     "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
            assert "tools" in info["capabilities"], info
            names = {tool["name"] for tool in request(server, 2, "tools/list")["tools"]}
            definitions = {tool["name"]: tool for tool in request(server, 17, "tools/list")["tools"]}
            assert "not correctly implemented" in definitions["rte_sim"]["description"]
            for name in ("rte_flash", "rte_device_telemetry", "rte_device_signal",
                         "rte_device_history", "rte_device_string_history",
                         "rte_device_command_response", "rte_device_mode"):
                assert name in names, name
            resources = request(server, 11, "resources/list")["resources"]
            assert any(item["uri"] == "rte://workspace/Assets/Examples/foc_demo.json"
                       for item in resources), resources
            graph = request(server, 12, "resources/read",
                            {"uri": "rte://workspace/Assets/Examples/foc_demo.json"})
            assert graph["contents"][0]["mimeType"] == "application/json", graph
            invalid = request(server, 13, "tools/call", {"name": "rte_flash",
                              "arguments": {"firmware": str(firmware), "target": "invalid"}})
            assert invalid["isError"], invalid
            ports = request(server, 3, "tools/call", {"name": "rte_bridge_ports",
                            "arguments": {}})
            assert str(bridge) in ports["content"][0]["text"], ports
            result = request(server, 4, "tools/call", {"name": "rte_flash",
                             "arguments": {"firmware": str(firmware),
                                           "serial": str(bridge),
                                           "programmer": str(programmer)}})
            assert not result.get("isError"), result
            assert result["structuredContent"]["success"], result
            assert received == ["BOOTLOADER", "APP"], received
            main_args = json.loads(log.read_text().splitlines()[0])
            assert "port=" + str(bridge) in main_args, main_args
            assert "br=460800" in main_args and "P=EVEN" in main_args, main_args
            coproc = request(server, 5, "tools/call", {"name": "rte_flash",
                              "arguments": {"firmware": str(firmware), "target": "coproc",
                                            "programmer": str(programmer)}})
            assert not coproc.get("isError"), coproc
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            assert ["-c", "port=usb1", "-g", "0x08001235"] == calls[-1], calls
            assert "0x08000000" in calls[-2], calls
            failed = request(server, 14, "tools/call", {"name": "rte_flash",
                             "arguments": {"firmware": str(failing_firmware),
                                           "serial": str(bridge),
                                           "programmer": str(programmer), "attempts": 2}})
            assert failed.get("isError"), failed
            assert received[-3:] == ["BOOTLOADER", "BOOTLOADER", "APP"], received
            telemetry = request(server, 6, "tools/call", {"name": "rte_device_telemetry",
                                "arguments": {}})
            assert '"phase_current_a": 1.25' in telemetry["content"][0]["text"], telemetry
            signal = request(server, 7, "tools/call", {"name": "rte_device_signal",
                             "arguments": {"signal": "state"}})
            assert '"idle"' in signal["content"][0]["text"], signal
            history = request(server, 8, "tools/call", {"name": "rte_device_history",
                              "arguments": {"signal": "phase_current_a", "limit": 10}})
            assert '"time_s": 0.1' in history["content"][0]["text"], history
            strings = request(server, 10, "tools/call", {"name": "rte_device_string_history",
                              "arguments": {"signal": "state", "limit": 10}})
            assert '"idle"' in strings["content"][0]["text"], strings
            reply = request(server, 9, "tools/call", {"name": "rte_device_command_response",
                            "arguments": {"command": "get currents", "timeout_ms": 100}})
            assert "OK: get currents" in reply["content"][0]["text"], reply
            assert session_server.commands == ["get currents"], session_server.commands
            mode = request(server, 15, "tools/call", {"name": "rte_device_mode",
                           "arguments": {}})["structuredContent"]
            assert mode["state"] == "app_responding", mode
            bridge_mode[0] = "BOOT_8E1"
            mode = request(server, 16, "tools/call", {"name": "rte_device_mode",
                           "arguments": {}})["structuredContent"]
            assert mode["state"] == "bootloader_selected", mode
            session_server.transport = "tcp"
            mode = request(server, 18, "tools/call", {"name": "rte_device_mode",
                           "arguments": {"serial": str(bridge), "control_port": str(control),
                                         "probe_bootloader": True,
                                         "programmer": str(programmer)}})["structuredContent"]
            assert mode["state"] == "bootloader_responding", mode
            assert json.loads(log.read_text().splitlines()[-1]) == [
                "-c", "port=" + str(bridge), "br=460800", "P=EVEN", "db=8", "sb=1"]
            bridge_mode[0] = "APP_8N1"

            def cobs_encode(data):
                out = bytearray([0])
                code_index = 0
                code = 1
                for byte in data:
                    if byte == 0:
                        out[code_index] = code
                        code_index = len(out)
                        out.append(0)
                        code = 1
                    else:
                        out.append(byte)
                        code += 1
                out[code_index] = code
                return bytes(out) + b"\x00"

            def app_writer():
                sequence = 1
                while not stop.is_set():
                    header = struct.pack("<IBBHII", 0x544C4D31, 1, 1, 0, sequence, 0)
                    packet = header + struct.pack("<H", binascii.crc_hqx(header, 0xFFFF))
                    try:
                        os.write(bridge_master, cobs_encode(packet))
                    except OSError:
                        return
                    sequence += 1
                    time.sleep(0.02)

            app_worker = threading.Thread(target=app_writer, daemon=True)
            app_worker.start()
            cli = subprocess.run([str(executable), "--format", "json", "device", "mode",
                                  "--serial", str(bridge), "--control-port", str(control),
                                  "--session", str(descriptor)], capture_output=True,
                                 text=True, env=env, check=True, timeout=5)
            report = json.loads(cli.stdout)["events"][0]
            assert report["operation"] == "mode", cli.stdout
            assert report["result"]["state"] == "app_responding", cli.stdout
            assert report["result"]["observation_source"] == "passive UART frame sample"
        finally:
            server.stdin.close()
            server.wait(timeout=5)
            stop.set()
            os.close(bridge_master)
            os.close(bridge_slave)
            os.close(control_master)
            os.close(control_slave)
            session_server.shutdown()
            session_server.server_close()
            assert server.returncode == 0, server.stderr.read()


if __name__ == "__main__":
    main()
