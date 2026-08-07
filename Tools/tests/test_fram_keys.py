#!/usr/bin/env python3
"""Unit tests for Tools/fram_keys.py.

These tests exercise the parsing and command-generation logic using a fake
transport; no real serial device or firmware is required.
"""

import io
import json
import os
import sys
import unittest
from typing import Dict, List

# Add the Tools/ directory (parent of this test file) so we can import
# fram_keys regardless of the current working directory.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fram_keys


class FakeInterface:
    """In-memory device interface that records commands and returns canned output."""

    def __init__(self, responses: Dict[str, List[str]]):
        self.responses = responses
        self.sent: List[str] = []

    def send_command(self, line: str) -> List[str]:
        self.sent.append(line)
        return self.responses.get(line, [])


class TestFramKeysClient(unittest.TestCase):
    def test_read_keys_parses_config_list_output(self):
        responses = {
            "config list": [
                "[SHELL] stored config keys (3):",
                "[SHELL]   Motor.Temp.Winding.Limit = 120.0000",
                "[SHELL]   Hw.Temp.Board.Offset = 0.0040",
                "[SHELL]   Can.Proto.AllowCmd = 1.0000",
            ]
        }
        client = fram_keys.FramKeysClient(FakeInterface(responses))
        keys = client.read_keys()
        self.assertEqual(keys["Motor.Temp.Winding.Limit"], 120.0)
        self.assertEqual(keys["Hw.Temp.Board.Offset"], 0.004)
        self.assertEqual(keys["Can.Proto.AllowCmd"], 1.0)
        self.assertEqual(len(keys), 3)

    def test_read_keys_tolerates_non_numeric_and_noise(self):
        responses = {
            "config list": [
                "random telemetry noise",
                "[SHELL] stored config keys (2):",
                "[SHELL]   Key.One = 1.5000",
                "[SHELL]   Key.Two = not_a_number",
                "[SHELL]   Key.Three = 2.0000",
            ]
        }
        client = fram_keys.FramKeysClient(FakeInterface(responses))
        keys = client.read_keys()
        self.assertEqual(keys, {"Key.One": 1.5, "Key.Three": 2.0})

    def test_write_keys_sends_set_and_save(self):
        interface = FakeInterface({})
        client = fram_keys.FramKeysClient(interface)
        client.write_keys({"Key.A": 1.5, "Key.B": -2.0})
        self.assertEqual(interface.sent, [
            "config set Key.A 1.5",
            "config save Key.A",
            "config set Key.B -2",
            "config save Key.B",
        ])

    def test_write_keys_dry_run_does_not_send(self):
        interface = FakeInterface({})
        client = fram_keys.FramKeysClient(interface)
        client.write_keys({"Key.A": 1.0}, dry_run=True)
        self.assertEqual(interface.sent, [])

    def test_delete_all_keys(self):
        interface = FakeInterface({})
        client = fram_keys.FramKeysClient(interface)
        client.delete_all_keys()
        self.assertEqual(interface.sent, ["config deleteall"])


class TestCommandLine(unittest.TestCase):
    def setUp(self):
        self._old_stdout = sys.stdout
        self._old_stderr = sys.stderr

    def tearDown(self):
        sys.stdout = self._old_stdout
        sys.stderr = self._old_stderr

    def _run_with_fake(self, argv: List[str], responses: Dict[str, List[str]]) -> int:
        interface = FakeInterface(responses)
        # Patch interface construction to return our fake.
        original_make = fram_keys.make_interface

        def fake_make_interface(args):
            return interface

        fram_keys.make_interface = fake_make_interface
        try:
            return fram_keys.main(argv)
        finally:
            fram_keys.make_interface = original_make

    def _capture_stdout(self):
        captured = io.StringIO()
        sys.stdout = captured
        return captured

    def test_save_command_writes_json(self):
        responses = {
            "config list": [
                "[SHELL] stored config keys (1):",
                "[SHELL]   My.Key = 3.1400",
            ]
        }
        path = "test_save_output.json"
        captured = self._capture_stdout()
        try:
            rc = self._run_with_fake(["--port", "/dev/fake", "save", path], responses)
            sys.stdout = self._old_stdout
            self.assertEqual(rc, 0)
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data, {"My.Key": 3.14})
            self.assertIn("Saved 1 key(s)", captured.getvalue())
        finally:
            import os
            sys.stdout = self._old_stdout
            if os.path.exists(path):
                os.remove(path)

    def test_load_command_reads_json_and_sends_commands(self):
        path = "test_load_input.json"
        captured = self._capture_stdout()
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"Key.A": 1.0, "Key.B": 2.5}, f)
            rc = self._run_with_fake(["--port", "/dev/fake", "load", path], {})
            sys.stdout = self._old_stdout
            self.assertEqual(rc, 0)
            self.assertIn("Loaded 2 key(s)", captured.getvalue())
        finally:
            import os
            sys.stdout = self._old_stdout
            if os.path.exists(path):
                os.remove(path)

    def test_load_with_clear_sends_deleteall_first(self):
        path = "test_load_clear_input.json"
        captured = self._capture_stdout()
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"Key.A": 1.0}, f)
            rc = self._run_with_fake(["--port", "/dev/fake", "load", "--clear", path], {})
            sys.stdout = self._old_stdout
            self.assertEqual(rc, 0)
            self.assertIn("Loaded 1 key(s)", captured.getvalue())
        finally:
            import os
            sys.stdout = self._old_stdout
            if os.path.exists(path):
                os.remove(path)

    def test_load_rejects_non_object_json(self):
        path = "test_load_bad_input.json"
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump([1, 2, 3], f)
            captured = io.StringIO()
            sys.stdout = captured
            rc = self._run_with_fake(["--port", "/dev/fake", "load", path], {})
            sys.stdout = self._old_stdout
            self.assertEqual(rc, 1)
        finally:
            import os
            if os.path.exists(path):
                os.remove(path)

    def test_load_dry_run_works_without_connection(self):
        path = "test_load_dry_run_input.json"
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"Key.A": 1.0}, f)
            captured = io.StringIO()
            sys.stdout = captured
            rc = fram_keys.main(["load", "--dry-run", path])
            sys.stdout = self._old_stdout
            self.assertEqual(rc, 0)
            output = captured.getvalue()
            self.assertIn("config set Key.A 1", output)
            self.assertIn("config save Key.A", output)
            self.assertIn("Would load 1 key(s)", output)
        finally:
            import os
            if os.path.exists(path):
                os.remove(path)

    def test_list_command_prints_sorted_keys(self):
        responses = {
            "config list": [
                "[SHELL] stored config keys (2):",
                "[SHELL]   Z.Key = 1.0000",
                "[SHELL]   A.Key = 2.0000",
            ]
        }
        captured = io.StringIO()
        sys.stdout = captured
        rc = self._run_with_fake(["--http", "http://localhost:8080", "list"], responses)
        sys.stdout = self._old_stdout
        self.assertEqual(rc, 0)
        self.assertEqual(captured.getvalue().strip(), "A.Key = 2\nZ.Key = 1")


if __name__ == "__main__":
    unittest.main()
