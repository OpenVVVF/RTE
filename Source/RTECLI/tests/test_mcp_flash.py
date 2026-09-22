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
                elif method == "device.build_info":
                    value = {"verified": True, "graph": "foc_demo", "graph_hash": "deadbeef",
                             "nodes": [{"id": "foc", "type": "Control.FOC"}]}
                elif method == "device.catalog":
                    requested = message["params"].get("signal", "")
                    value = {"build_verified": True,
                             "signals": ([{"name": "phase_current_a", "state": "live",
                                           "unit": "A", "source_node": "current_sensor"}]
                                         if requested in ("", "phase_current_a") else []),
                             "state": "not_in_build" if requested == "missing_signal" else "live"}
                elif method == "device.control_status":
                    value = {"fresh": True, "state": "idle", "fault_names": "none",
                             "pwm_moe": 0, "gate_ready": 1}
                elif method == "device.snapshot":
                    value = {"bundle": "foc", "values": {"phase_current_a": {
                        "value": 1.25, "kind": "number", "fresh": True, "age_s": 0.1}},
                        "missing": [], "frame_age_s": 0.1}
                elif method == "device.histories":
                    value = {"series": {key: [{"time_s": 0.0, "value": 0.0},
                                               {"time_s": 0.5, "value": 1.25},
                                               {"time_s": 1.0, "value": 0.5}]
                                        for key in message["params"]["signals"]},
                             "missing": [], "window_end_s": 1.0, "window_s": 1.0}
                elif method == "device.status":
                    value = {"connected": True, "device_port": str(bridge),
                             "transport": self.server.transport, "suspended": False}
                elif method in ("device.history", "device.string_history"):
                    value = {"signal": message["params"]["signal"],
                             "samples": [{"time_s": 0.1,
                                          "value": "idle" if method == "device.string_history" else 1.25}]}
                elif method == "device.command":
                    self.server.commands.append(message["params"]["command"])
                    self.server.command_sources.append(message["params"].get("source"))
                    value = {"sent": True, "command": self.server.commands[-1],
                             "console_since": 4}
                elif method == "device.console":
                    lines = [{"seq": 5, "text": "[MCP] sent"}]
                    if self.server.commands and self.server.commands[-1] == "help":
                        lines += [
                            {"seq": 6, "text": "[SHELL] === Command Reference "
                                               + ("(3 commands)" if self.server.incomplete_help
                                                  else "(2 commands)") + " ==="},
                            {"seq": 7, "text": "[SHELL] help                        - List commands"},
                            {"seq": 8, "text": "[SHELL] freq     <hz>            - Set frequency"},
                            {"seq": 9, "text": "[SHELL]         hz:0.0-100.0 Hz"},
                            {"seq": 10, "text": "[SHELL] ========================="},
                        ]
                    elif self.server.commands and self.server.commands[-1] == "spikes":
                        lines.append({"seq": 6, "text":
                                      "[SHELL] spikes: capture #1 (* = trigger), 64 samples @ 5 kHz"})
                        for index in range(64):
                            marker = "*" if index == 47 else " "
                            lines.append({"seq": index + 7, "text":
                                f"[SHELL] spk{marker}{index:02d} t=100 iu={index / 10:.1f} "
                                f"iv={index / 20:.1f} ang={index:.1f} dang=+1.00 "
                                "du=10.0 dv=20.0 dw=30.0 sin=123 cos=456"})
                    elif self.server.commands and self.server.commands[-1] != "silent":
                        lines.append({"seq": 6, "text": "OK: " + self.server.commands[-1]})
                    value = {"lines": lines if self.server.commands else [],
                             "latest_seq": lines[-1]["seq"]}
                elif method == "automation.activity":
                    self.server.activities.append(message["params"])
                    value = {"recorded": True}
                else:
                    value = {"connected": True, "device_port": "mock"}
                self.wfile.write((json.dumps({"ok": True, "result": value}) + "\n").encode())

        session_server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), SessionHandler)
        session_server.daemon_threads = True
        session_server.commands = []
        session_server.command_sources = []
        session_server.activities = []
        session_server.good_frames = 100
        session_server.transport = "serial"
        session_server.incomplete_help = False
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
            for tool in definitions.values():
                assert isinstance(tool["inputSchema"]["properties"], dict), tool["name"]
            for name in ("rte_flash", "rte_device_telemetry", "rte_device_signal",
                         "rte_device_history", "rte_device_string_history",
                         "rte_device_command_response", "rte_device_mode",
                         "rte_build_info", "rte_signal_info", "rte_control_status",
                         "rte_device_histories", "rte_device_trends",
                         "rte_device_snapshot", "rte_spike_capture",
                         "rte_device_commands"):
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
            assert [entry["action"] for entry in session_server.activities] == ["resources/read"]
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
            missing_signal = request(server, 28, "tools/call", {"name": "rte_device_signal",
                                     "arguments": {"signal": "missing_signal"}})
            assert missing_signal["isError"], missing_signal
            assert missing_signal["structuredContent"]["code"] == "not_in_build"
            history = request(server, 8, "tools/call", {"name": "rte_device_history",
                              "arguments": {"signal": "phase_current_a", "limit": 10}})
            assert '"time_s": 0.1' in history["content"][0]["text"], history
            strings = request(server, 10, "tools/call", {"name": "rte_device_string_history",
                              "arguments": {"signal": "state", "limit": 10}})
            assert '"idle"' in strings["content"][0]["text"], strings
            build_info = request(server, 20, "tools/call", {"name": "rte_build_info",
                                 "arguments": {}})
            assert build_info["structuredContent"]["graph"] == "foc_demo", build_info
            catalog = request(server, 21, "tools/call", {"name": "rte_signal_info",
                              "arguments": {"signal": "phase_current_a"}})
            assert catalog["structuredContent"]["signals"][0]["unit"] == "A", catalog
            control_status = request(server, 22, "tools/call", {"name": "rte_control_status",
                                     "arguments": {}})
            assert control_status["structuredContent"]["gate_ready"] == 1, control_status
            snapshot = request(server, 26, "tools/call", {"name": "rte_device_snapshot",
                               "arguments": {"signals": ["phase_current_a"]}})
            assert snapshot["structuredContent"]["values"]["phase_current_a"]["fresh"]
            histories = request(server, 23, "tools/call", {"name": "rte_device_histories",
                                "arguments": {"signals": ["phase_current_a"],
                                              "window_s": 1.0}})
            assert len(histories["structuredContent"]["series"]["phase_current_a"]) == 3
            trends = request(server, 24, "tools/call", {"name": "rte_device_trends",
                             "arguments": {"signals": ["phase_current_a"],
                                           "window_s": 1.0}})
            assert trends["structuredContent"]["metrics"]["phase_current_a"]["rms"] > 0
            assert "phase_current_a" in trends["content"][0]["text"], trends
            bad_trends = request(server, 25, "tools/call", {"name": "rte_device_trends",
                                 "arguments": {"signals": [17]}})
            assert bad_trends["isError"], bad_trends
            reply = request(server, 9, "tools/call", {"name": "rte_device_command_response",
                            "arguments": {"command": "get currents", "timeout_ms": 100}})
            assert "OK: get currents" in reply["content"][0]["text"], reply
            assert reply["structuredContent"]["response_observed"], reply
            assert session_server.commands == ["get currents"], session_server.commands
            assert session_server.command_sources == ["mcp"], session_server.command_sources
            assert any(entry["action"] == "rte_flash" and entry["state"] == "started"
                       and str(firmware) in entry["detail"] for entry in session_server.activities)
            assert any(entry["action"] == "rte_flash" and entry["state"] == "failed"
                       for entry in session_server.activities)
            assert any(entry["action"] == "rte_device_command_response"
                       and entry["state"] == "completed" for entry in session_server.activities)
            assert all("get currents" not in entry["detail"]
                       for entry in session_server.activities)
            silent = request(server, 19, "tools/call", {"name": "rte_device_command_response",
                             "arguments": {"command": "silent", "timeout_ms": 0}})
            assert not silent["structuredContent"]["response_observed"], silent
            assert session_server.command_sources == ["mcp", "mcp"]
            cli_command = subprocess.run(
                [str(executable), "--format", "json", "device", "command",
                 "--session", str(descriptor), "--command", "get status"],
                capture_output=True, text=True, env=env, check=True, timeout=5)
            assert '"sent": true' in cli_command.stdout, cli_command.stdout
            assert session_server.command_sources == ["mcp", "mcp", "cli"]
            for args, expected in (
                (["build-info"], "foc_demo"),
                (["signals", "--filter", "phase"], "current_sensor"),
                (["control-status"], "gate_ready"),
                (["snapshot", "--signal", "phase_current_a"], "phase_current_a"),
                (["signal", "--signal", "phase_current_a"], "1.25"),
                (["histories", "--signal", "phase_current_a", "--window-s", "1"],
                 "window_end_s"),
            ):
                cli_read = subprocess.run(
                    [str(executable), "--format", "json", "device", *args,
                     "--session", str(descriptor)],
                    capture_output=True, text=True, env=env, check=True, timeout=5)
                assert expected in cli_read.stdout, cli_read.stdout
            manual_tool = subprocess.run(
                [str(executable), "--format", "json", "tool", "rte_device_trends",
                 "--arguments", '{"signals":["phase_current_a"],"window_s":1}',
                 "--session", str(descriptor)],
                capture_output=True, text=True, env=env, check=True, timeout=5)
            assert "phase_current_a" in manual_tool.stdout, manual_tool.stdout
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
            spike = request(server, 27, "tools/call", {"name": "rte_spike_capture",
                            "arguments": {"timeout_ms": 1000}})
            assert spike["structuredContent"]["complete"], spike
            assert len(spike["structuredContent"]["samples"]) == 64, spike
            assert spike["structuredContent"]["samples"][47]["trigger"], spike
            assert "iu_a" in spike["content"][0]["text"], spike
            commands = request(server, 29, "tools/call", {"name": "rte_device_commands",
                               "arguments": {"timeout_ms": 1000}})
            catalog = commands["structuredContent"]
            assert catalog["complete"] and catalog["count"] == 2, catalog
            assert catalog["count_verified"] and catalog["argument_ranges_complete"], catalog
            assert catalog["commands"][1]["arguments"][0] == {
                "name": "hz", "required": True, "range": "0.0-100.0 Hz", "type": "float"}, catalog
            assert session_server.commands[-1] == "help"
            session_server.incomplete_help = True
            incomplete = request(server, 30, "tools/call", {"name": "rte_device_commands",
                                 "arguments": {"timeout_ms": 1000}})
            assert incomplete["isError"] and not incomplete["structuredContent"]["complete"]
            assert incomplete["structuredContent"]["expected_count"] == 3
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
