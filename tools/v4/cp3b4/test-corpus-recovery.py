"""Portable regression tests for batch failure isolation and durable progress."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("corpus_runner", Path(__file__).with_name("corpus-runner.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RecoveryTests(unittest.TestCase):
    def test_real_worker_failure_and_timeout_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            report = Path(temp) / "report.json"
            command = [sys.executable, "-c", "import sys; sys.stdout.buffer.write(b'bad byte: \\xff'); sys.exit(9)"]
            with patch.object(runner, "command_for_asset", return_value=command):
                result = runner.invoke_once(argparse.Namespace(timeout=5), {}, report)
            self.assertEqual(result["returnCode"], 9)
            self.assertIn("bad byte:", result["stdoutTail"])
            json.dumps(result)
            command = [sys.executable, "-c", "import time; print('before timeout', flush=True); time.sleep(5)"]
            with patch.object(runner, "command_for_asset", return_value=command):
                result = runner.invoke_once(argparse.Namespace(timeout=1), {}, report)
            self.assertTrue(result["timedOut"])
            self.assertIn("before timeout", result["stdoutTail"])
            json.dumps(result)

    def test_timeout_bytes_remain_json_serializable(self):
        args = argparse.Namespace(timeout=1)
        error = subprocess.TimeoutExpired(["tool"], 1, output=b"before timeout\xff", stderr=b"GPU diagnostic")
        with patch.object(runner, "command_for_asset", return_value=["tool"]), \
                patch.object(runner.subprocess, "run", side_effect=error):
            result = runner.invoke_once(args, {}, Path("unused.json"))
        self.assertTrue(result["timedOut"])
        self.assertIn("before timeout", result["stdoutTail"])
        json.dumps(result)

    def test_launch_failure_is_an_asset_result(self):
        with patch.object(runner, "command_for_asset", return_value=["tool"]), \
                patch.object(runner.subprocess, "run", side_effect=OSError("bad executable")):
            result = runner.invoke_once(argparse.Namespace(timeout=1), {}, Path("unused.json"))
        self.assertFalse(result["timedOut"])
        self.assertIn("bad executable", result["invocationErrors"][0])
        self.assertIsNone(result["returnCode"])

    def run_batch(self, root, interrupt=False):
        root = Path(root)
        tool = root / "tool.exe"
        tool.write_bytes(b"fixture identity, never executed")
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({"schema": runner.CORPUS_SCHEMA, "assets": [
            {"id": name, "nif": f"meshes/{name}.nif"} for name in ("broken", "working", "last")
        ]}), encoding="utf-8")
        output = root / "report.json"
        args = argparse.Namespace(tool=tool, manifest=manifest, output=output, data=[root], archive=[],
            determinism_runs=1, render_frames=0, render_id=[], timeout=1, encoding="win1252")
        visited = []

        def asset_test(args, asset, work_dir):
            # A complete report must already exist before each potentially
            # crashing process starts, even when no case has finished yet.
            partial = json.loads(output.read_text(encoding="utf-8"))
            self.assertFalse(partial["complete"])
            self.assertFalse(partial["passed"])
            self.assertEqual(len(partial["assets"]), len(visited))
            if interrupt and visited:
                raise KeyboardInterrupt
            visited.append(asset["id"])
            return {**asset, "passed": asset["id"] != "broken", "errors": ["fixture failure"]
                    if asset["id"] == "broken" else [], "toolReport": None, "runs": []}

        with patch.object(runner, "parse_args", return_value=args), \
                patch.object(runner, "run_asset", side_effect=asset_test):
            status = runner.main()
        return status, visited, json.loads(output.read_text(encoding="utf-8"))

    def test_failure_does_not_stop_later_cases_and_progress_is_saved(self):
        with tempfile.TemporaryDirectory() as temp:
            status, visited, report = self.run_batch(temp)
        self.assertEqual(status, 1)
        self.assertEqual(visited, ["broken", "working", "last"])
        self.assertTrue(report["complete"])
        self.assertEqual(report["summary"]["failed"], 1)
        self.assertEqual(report["summary"]["notTested"], 0)

    def test_interrupt_preserves_completed_cases_without_false_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            status, visited, report = self.run_batch(temp, interrupt=True)
        self.assertEqual(status, 130)
        self.assertEqual(visited, ["broken"])
        self.assertFalse(report["complete"])
        self.assertFalse(report["passed"])
        self.assertEqual(report["summary"]["notTested"], 2)


if __name__ == "__main__":
    unittest.main()
