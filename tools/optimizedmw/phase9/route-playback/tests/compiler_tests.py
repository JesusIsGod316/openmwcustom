"""Compiler integrity and unsupported-route admission checks; no game required."""
from collections import Counter
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("compile_route", Path(__file__).resolve().parents[1] / "compile_route.py")
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


def fixture_pose():
    return {"position": {"x": -14568.84375, "y": -12784.79492, "z": 488.34},
            "cell": {"exterior": True, "id": "Esm3ExteriorCell:-2:-2", "name": "Balmora"},
            "camera": {"yaw": 3.13, "pitch": 0.2, "fov": 1.65, "mode": 1,
                       "extra_yaw": 0, "extra_pitch": 0, "roll": 0}}


def fixture_policy():
    return {"qualified": True, "enabled": True, "ready": True, "failed": False,
            "combat_before_start": 0, "combat_during_recording": 0, "failure_count": 0, "pending_disable": 0}


class CompilerChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="optimizedmw-playback-compiler-")
        self.root = Path(self.temp.name)
        self.capture = self.root / "capture"
        self.capture.mkdir()
        self.session = "a" * 32
        (self.capture / "Start.omwsave").write_bytes(b"fixture-private-start-not-engine-proof")
        (self.capture / "SESSION.json").write_text(json.dumps({"schema": 1, "session": self.session, "route": "balmora"}), encoding="utf-8")
        self.rows = [
            {"event": "ready"},
            {"event": "start", "route_name": "balmora", "pose": fixture_pose(), "checkpoint_pose": fixture_pose(), "policy": fixture_policy()},
            {"event": "pause", "simulation": 0, "f": 1, "paused": False},
            {"event": "cell", "f": 1, "cell": fixture_pose()["cell"]},
            self.control(0, 1), self.sample(0, 1),
            {"event": "checkpoint", "f": 2, "index": 1, "pose": fixture_pose()},
            self.control(1, 3), self.sample(1, 3), self.sample(2, 5),
            {"event": "finish", "route_name": "balmora", "duration_simulation": 2, "duration_wall": 100,
             "flags": {"complete": True, "policy_qualified": True, "policy_ready": True, "door_observer_ready": True},
             "policy": fixture_policy(), "activation_coverage": {"input_attempts": 0, "actual_known": 0, "actual_unknown": 0}}]
        self.write_fixture()

    def tearDown(self):
        self.temp.cleanup()

    def control(self, timestamp, frame):
        return {"event": "control", "simulation": timestamp, "f": frame,
                "controls": {"movement": 0, "side_movement": 0, "run": True, "sneak": False, "jump": False,
                             "use": 0, "yaw_change": 0, "pitch_change": 0},
                "actions": {"forward": 0, "backward": 0, "left": 0, "right": 0, "run": False, "sneak": False, "use": False}}

    def sample(self, timestamp, frame):
        return {"event": "sample", "simulation": timestamp, "f": frame, "paused": False, **fixture_pose()}

    def write_fixture(self, preserve_sequence=False):
        for index, row in enumerate(self.rows, 1):
            if not preserve_sequence:
                row["seq"] = index
            row.update(schema=1, session=self.session, t=index * 100)
        counts = dict(Counter(row["event"] for row in self.rows))
        self.rows[-1]["expected_records"] = len(self.rows)
        self.rows[-1]["counts"] = {key: value for key, value in counts.items() if key != "finish"}
        (self.capture / "route.jsonl").write_text("\n".join(json.dumps(row) for row in self.rows) + "\n", encoding="utf-8")
        summary = {"schema": 1, "session": self.session, "complete": True, "qualification": "RECORDED_NOT_REPLAY_VALIDATED",
                   "exit_code": 0, "protected_inputs_unchanged": True, "errors": [], "record_count": len(self.rows),
                   "events": counts, "duration_seconds": self.rows[-1]["duration_wall"],
                   "route_file": compiler.identity(self.capture / "route.jsonl"),
                   "starting_save": {"route_copy": compiler.identity(self.capture / "Start.omwsave")}}
        (self.capture / "SUMMARY.json").write_text(json.dumps(summary), encoding="utf-8")

    def compile(self):
        return compiler.compile_route(self.capture, self.root / "timeline.lua", self.root / "receipt.json")

    def test_valid_fixture_uses_simulation_and_interpolates_marker_frame(self):
        before = [compiler.identity(path) for path in self.capture.iterdir()]
        result = self.compile()
        output = (self.root / "timeline.lua").read_text(encoding="utf-8")
        self.assertEqual(result["duration_simulation"], 2)
        self.assertIn("index=1,t=0.5", output)
        self.assertIn("duration=2", output)
        self.assertNotIn("t=500", output)
        self.assertEqual(before, [compiler.identity(path) for path in self.capture.iterdir()])
        self.assertEqual(result["inputs_before"], result["inputs_after"])
        self.assertEqual(result["qualification"], "COMPILED_NOT_REPLAY_VALIDATED")

    def test_corrupted_raw_route_refused_before_output(self):
        with (self.capture / "route.jsonl").open("ab") as stream:
            stream.write(b" ")
        with self.assertRaisesRegex(compiler.CompileError, "Route identity mismatch"):
            self.compile()
        self.assertFalse((self.root / "timeline.lua").exists())

    def test_corrupted_start_save_refused(self):
        (self.capture / "Start.omwsave").write_bytes(b"changed")
        with self.assertRaisesRegex(compiler.CompileError, "Starting save identity mismatch"):
            self.compile()

    def test_discontinuous_sequence_refused(self):
        self.rows[4]["seq"] = 99
        self.write_fixture(preserve_sequence=True)
        with self.assertRaisesRegex(compiler.CompileError, "Discontinuous"):
            self.compile()

    def test_mixed_session_refused(self):
        self.rows[4]["session"] = "b" * 32
        path = self.capture / "route.jsonl"
        path.write_text("\n".join(json.dumps(row) for row in self.rows), encoding="utf-8")
        summary = json.loads((self.capture / "SUMMARY.json").read_text())
        summary["route_file"] = compiler.identity(path)
        (self.capture / "SUMMARY.json").write_text(json.dumps(summary))
        with self.assertRaisesRegex(compiler.CompileError, "mixed schema/session"):
            self.compile()

    def test_use_action_refused(self):
        self.rows[4]["controls"]["use"] = 1
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Use/attack"):
            self.compile()

    def test_jump_action_refused_before_silent_omission(self):
        self.rows[4]["controls"]["jump"] = True
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Jump action"):
            self.compile()

    def test_activation_event_refused(self):
        self.rows.insert(-1, {"event": "activation"})
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Unsupported action/transition"):
            self.compile()

    def test_third_person_camera_refused(self):
        self.rows[5]["camera"]["mode"] = 3
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "first-person"):
            self.compile()

    def test_interior_transition_refused(self):
        self.rows[5]["cell"]["exterior"] = False
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "exterior cells"):
            self.compile()

    def test_paused_transition_refused(self):
        self.rows[2]["paused"] = True
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Paused/menu"):
            self.compile()

    def test_nonfinite_input_refused(self):
        self.rows[5]["position"]["x"] = float("nan")
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Non-finite"):
            self.compile()

    def test_duplicate_json_keys_refused(self):
        with self.assertRaisesRegex(compiler.CompileError, "Duplicate JSON"):
            compiler.read_json('{"complete": false, "complete": true}')

    def test_boolean_exit_code_is_not_native_zero_exit_proof(self):
        path = self.capture / "SUMMARY.json"
        summary = json.loads(path.read_text())
        summary["exit_code"] = False
        path.write_text(json.dumps(summary))
        with self.assertRaisesRegex(compiler.CompileError, "not an intact"):
            self.compile()

    def test_angles_wrapping_pi_are_accepted_without_rewriting(self):
        self.rows[5]["camera"]["yaw"] = 3.1415
        self.rows[8]["camera"]["yaw"] = -3.1415
        self.write_fixture()
        self.compile()
        output = (self.root / "timeline.lua").read_text()
        self.assertIn("yaw=3.1415", output)
        self.assertIn("yaw=-3.1415", output)

    def test_out_of_range_pitch_refused(self):
        self.rows[5]["camera"]["pitch"] = 3.14
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "camera pitch"):
            self.compile()

    def test_unqualified_combat_policy_refused(self):
        self.rows[-1]["policy"]["combat_during_recording"] = 1
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "combat_during_recording"):
            self.compile()

    def test_marker_without_frame_coverage_refused(self):
        self.rows[6]["f"] = 999
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "Marker has no"):
            self.compile()

    def test_nonmonotonic_simulation_refused(self):
        self.rows[8]["simulation"] = -1
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "simulation timestamp"):
            self.compile()

    def test_duration_over_budget_refused(self):
        self.rows[-1]["duration_simulation"] = 181
        self.write_fixture()
        with self.assertRaisesRegex(compiler.CompileError, "simulation duration"):
            self.compile()

    def test_existing_output_preserved(self):
        path = self.root / "timeline.lua"
        path.write_text("keep me")
        with self.assertRaisesRegex(compiler.CompileError, "Preserving existing"):
            self.compile()
        self.assertEqual(path.read_text(), "keep me")

    def test_output_in_original_capture_refused(self):
        with self.assertRaisesRegex(compiler.CompileError, "outside original capture"):
            compiler.compile_route(self.capture, self.capture / "timeline.lua", self.root / "receipt.json")

    def test_lua_string_quoting_does_not_emit_executable_fragment(self):
        self.assertEqual(compiler.lua('"\\\n\r\x00é'), '"\\"\\\\\\010\\013\\000é"')


if __name__ == "__main__":
    unittest.main(verbosity=2)
