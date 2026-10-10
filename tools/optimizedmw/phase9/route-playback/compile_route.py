"""Compile a qualified acquisition into bounded, simulation-timed preview inputs.

This compiler does not qualify physical playback or benchmark performance.
"""
from __future__ import annotations

import argparse
import bisect
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sys


MAX_ROUTE_BYTES = 16 * 1024 * 1024
MAX_RECORDS = 60000
MAX_DURATION = 180.0
MAX_WALL_DURATION = 300.0
ALLOWED_ROUTES = {"balmora", "existing"}
NATIVE_MOVEMENT_COHORT = {"model": "native_input_displacement_with_visual_animations", "private_override": True}
ADDITIVE_CAMERA_COHORT = {"model": "recorded_additive_first_person_v1"}
CAMERA_LAYER_ANGLES = ("extra_yaw", "extra_pitch", "roll", "extra_roll")
SUPPORTED_MOVEMENT_TRIGGERS = {"AutoMove", "AlwaysRun", "ToggleSneak"}
ALLOWED_EVENTS = {"ready", "start", "finish", "control", "sample", "checkpoint", "pause", "cell", "policy", "trigger"}


class CompileError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CompileError(message)


def identity(path):
    require(path.is_file() and not path.is_symlink(), f"Missing or linked input: {path}")
    data = path.read_bytes()
    return {"path": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def read_json(data):
    def reject_constant(value):
        raise CompileError(f"Non-finite JSON constant: {value}")
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "Duplicate JSON object key: " + key)
            result[key] = value
        return result
    try:
        result = json.loads(data, parse_constant=reject_constant, object_pairs_hook=unique_object)
    except (ValueError, UnicodeError) as exc:
        raise CompileError(f"Invalid JSON: {exc}") from exc
    finite_tree(result)
    return result


def finite_tree(value, depth=0):
    require(depth <= 20, "Input nesting exceeds 20 levels")
    if isinstance(value, float):
        require(math.isfinite(value), "Non-finite numeric input")
    elif isinstance(value, dict):
        for item in value.values():
            finite_tree(item, depth + 1)
    elif isinstance(value, list):
        require(len(value) <= MAX_RECORDS, "Input array exceeds record limit")
        for item in value:
            finite_tree(item, depth + 1)


def number(value, label, minimum=-1e8, maximum=1e8):
    require(type(value) in (int, float) and math.isfinite(value) and minimum <= value <= maximum,
            f"Invalid {label}")
    return value


def text(value, label, allow_empty=False):
    require(isinstance(value, str) and len(value) <= 512 and (allow_empty or bool(value))
            and not any(ord(char) < 32 for char in value), f"Invalid {label}")
    return value


def boolean(value, label):
    require(type(value) is bool, f"Invalid {label}")
    return value


def checked_identity(actual, expected, label):
    require(isinstance(expected, dict), f"Missing {label} identity")
    require(expected.get("sha256", "").lower() == actual["sha256"] and expected.get("bytes") == actual["bytes"],
            f"{label} identity mismatch")
    require(Path(expected.get("path", "")).resolve() == Path(actual["path"]), f"{label} identity path mismatch")


def policy_qualified(policy, label):
    require(isinstance(policy, dict), f"Missing {label} policy")
    require(policy.get("qualified") is True and policy.get("ready") is True and policy.get("enabled") is True
            and policy.get("failed") is False, f"Unqualified {label} policy")
    for key in ("combat_before_start", "combat_during_recording", "failure_count", "pending_disable"):
        require(type(policy.get(key)) is int and policy[key] == 0, f"Nonzero or unavailable {label} {key}")


def checkpoint_authenticated(summary, start, session_id, route_name, copied_identity):
    """Authenticate the acknowledged new slot without reading a mutable runtime save."""
    checkpoint = start.get("checkpoint_save")
    require(isinstance(checkpoint, dict), "Missing starting checkpoint metadata")
    request = checkpoint.get("request")
    request_pattern = re.escape(session_id) + r":start(?::(?:[2-9]|[1-9][0-9]{1,5}))?"
    require(isinstance(request, str) and re.fullmatch(request_pattern, request) is not None
            and checkpoint.get("description") == f"OptimizedMW route {route_name} {session_id}"
            and checkpoint.get("source") == "private_menu_new_save_slot",
            "Starting checkpoint is not the authenticated new private route slot")
    for key in ("directory", "filename"):
        part = checkpoint.get(key)
        require(isinstance(part, str) and 0 < len(part) <= 256
                and re.search(r"[\\/:\r\n]|^\.{1,2}$|[. ]$", part) is None,
                "Unsafe checkpoint save identity")
    require(checkpoint["filename"].lower().endswith(".omwsave"), "Checkpoint is not an omwsave")
    starting_save = summary.get("starting_save", {})
    require(summary.get("checkpoint_save") == starting_save.get("metadata") == checkpoint,
            "Raw/summary starting checkpoint metadata disagree")
    original = starting_save.get("source")
    require(isinstance(original, dict) and isinstance(original.get("sha256"), str)
            and original["sha256"].lower() == copied_identity["sha256"]
            and type(original.get("bytes")) is int and original["bytes"] == copied_identity["bytes"],
            "Original/copy starting save identity mismatch")
    return dict(checkpoint)


def cell(value):
    require(isinstance(value, dict) and value.get("exterior") is True, "Playback currently supports exterior cells only")
    cell_id = text(value.get("id"), "cell ID")
    match = re.fullmatch(r"Esm3ExteriorCell:(-?\d+):(-?\d+)", cell_id)
    require(match is not None, "Unsupported exterior cell identity")
    grid_x, grid_y = (int(part) for part in match.groups())
    require(all(-2147483648 <= coordinate <= 2147483647 for coordinate in (grid_x, grid_y))
            and cell_id == f"Esm3ExteriorCell:{grid_x}:{grid_y}", "Noncanonical exterior cell identity")
    if "grid_x" in value or "grid_y" in value:
        require(type(value.get("grid_x")) is int and type(value.get("grid_y")) is int
                and value["grid_x"] == grid_x and value["grid_y"] == grid_y,
                "Exterior cell grid disagrees with identity")
    cell_name = text(value.get("name"), "cell name", allow_empty=True)
    return {"cell_id": cell_id, "cell_name": cell_name, "exterior": True, "grid_x": grid_x, "grid_y": grid_y}


def movement_cohort(route_name, start, finish, ready):
    """Do not infer acquisition movement semantics from a save or later settings."""
    start_cohort, finish_cohort = start.get("movement_cohort"), finish.get("movement_cohort")
    if start_cohort is None and finish_cohort is None and route_name == "balmora":
        require(ready.get("movement_cohort") is None, "Mixed acquisition movement cohort")
        return None, "legacy_unrecorded"
    require(isinstance(start_cohort, dict) and isinstance(finish_cohort, dict)
            and set(start_cohort) == set(finish_cohort) == set(NATIVE_MOVEMENT_COHORT)
            and start_cohort.get("model") == finish_cohort.get("model") == NATIVE_MOVEMENT_COHORT["model"]
            and start_cohort.get("private_override") is True and finish_cohort.get("private_override") is True,
            "Acquisition requires matching native movement cohort at start/finish")
    if ready.get("movement_cohort") is not None:
        require(type(ready["movement_cohort"]) is dict and set(ready["movement_cohort"]) == set(NATIVE_MOVEMENT_COHORT)
                and ready["movement_cohort"].get("model") == NATIVE_MOVEMENT_COHORT["model"]
                and ready["movement_cohort"].get("private_override") is True, "Mixed acquisition movement cohort")
    return dict(NATIVE_MOVEMENT_COHORT), "recorded_native"


def camera_layer_cohort(session, summary, ready, start, finish):
    """Admit camera layers only with an explicit, matching acquisition contract."""
    cohorts = [boundary.get("camera_layer_cohort") for boundary in (session, summary, ready, start, finish)]
    if all(cohort is None for cohort in cohorts):
        return None, "legacy_zero_layers"
    require(all(type(cohort) is dict and cohort == ADDITIVE_CAMERA_COHORT for cohort in cohorts),
            "Camera layer cohort must match SESSION/SUMMARY/READY/START/FINISH")
    return dict(ADDITIVE_CAMERA_COHORT), "recorded_additive"


def camera(value, layer_cohort=None):
    require(isinstance(value, dict), "Missing camera pose")
    require(type(value.get("mode")) is int and value["mode"] == 1, "Playback supports first-person camera mode only")
    result = {"yaw": number(value.get("yaw"), "camera yaw", -math.tau, math.tau),
            "pitch": number(value.get("pitch"), "camera pitch", -math.pi / 2, math.pi / 2),
            "fov": number(value.get("fov"), "camera FOV", 0.01, math.pi - 0.01), "mode": 1}
    if layer_cohort is not None:
        require(layer_cohort == ADDITIVE_CAMERA_COHORT, "Unknown camera layer cohort")
        for key in CAMERA_LAYER_ANGLES:
            result[key] = number(value.get(key), f"camera {key}", -math.pi, math.pi)
        offset = value.get("first_person_offset")
        require(type(offset) is dict and set(offset) == set("xyz"), "Invalid camera first-person offset")
        result["first_person_offset"] = {axis: number(offset[axis], "camera first-person offset " + axis, -1024, 1024)
                                         for axis in "xyz"}
    else:
        for key in CAMERA_LAYER_ANGLES:
            require(number(value.get(key, 0), f"camera {key}", -math.tau, math.tau) == 0,
                    "Playback does not support additional camera rotation layers without a recorded cohort")
        if "first_person_offset" in value:
            offset = value["first_person_offset"]
            require(type(offset) is dict and set(offset) == set("xyz")
                    and all(number(offset[axis], "camera first-person offset " + axis, -1024, 1024) == 0 for axis in "xyz"),
                    "Playback does not support first-person offsets without a recorded cohort")
    return result


def pose(value, layer_cohort=None):
    require(isinstance(value, dict) and isinstance(value.get("position"), dict), "Missing position pose")
    result = {"position": {axis: number(value["position"].get(axis), "position " + axis) for axis in "xyz"},
              "camera": camera(value.get("camera"), layer_cohort)}
    result.update(cell(value.get("cell")))
    return result


def resolved_controls(values):
    require(isinstance(values, dict)
            and set(values) <= {"movement", "side_movement", "run", "sneak", "jump", "use", "yaw_change", "pitch_change"},
            "Unknown or unavailable resolved control fields")
    require(type(values.get("use")) in (int, float) and values["use"] == 0, "Use/attack action is unsupported")
    require(values.get("jump") is False, "Jump action is unsupported by the current preview")
    for key in ("yaw_change", "pitch_change"):
        number(values.get(key, 0), key, -math.tau, math.tau)
    return {"movement": number(values.get("movement"), "movement", -1, 1),
            "side_movement": number(values.get("side_movement"), "side movement", -1, 1),
            **{key: boolean(values.get(key), key) for key in ("run", "sneak", "jump")}, "use": 0}


def lua(value):
    """Serialize data only; escape Lua strings without executable fragments."""
    if value is None:
        return "nil"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) in (int, float):
        number(value, "generated number", -1e16, 1e16)
        return repr(value)
    if isinstance(value, str):
        escaped = []
        for char in value:
            code = ord(char)
            if char in ('"', "\\"):
                escaped.append("\\" + char)
            elif code < 32 or code == 127:
                escaped.append("\\" + f"{code:03d}")
            else:
                escaped.append(char)
        return '"' + "".join(escaped) + '"'
    if isinstance(value, list):
        return "{" + ",".join(lua(item) for item in value) + "}"
    require(isinstance(value, dict), "Unsupported generated value")
    require(all(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) for key in value), "Unsafe generated table key")
    return "{" + ",".join(key + "=" + lua(item) for key, item in value.items()) + "}"


def compile_route(capture, output, receipt):
    capture, output, receipt = Path(capture).resolve(), Path(output).resolve(), Path(receipt).resolve()
    require(capture.is_dir(), "Capture directory is missing")
    require(output != receipt and not output.is_relative_to(capture) and not receipt.is_relative_to(capture),
            "Outputs must be distinct and outside original capture evidence")
    require(not output.exists() and not receipt.exists(), "Preserving existing compiled output/receipt")
    paths = [capture / name for name in ("SUMMARY.json", "SESSION.json", "route.jsonl", "Start.omwsave")]
    before = [identity(path) for path in paths]
    require(before[0]["bytes"] <= 1024 * 1024 and before[1]["bytes"] <= 8 * 1024 * 1024, "Metadata exceeds size bound")
    require(0 < before[2]["bytes"] <= MAX_ROUTE_BYTES, "Route exceeds 16 MiB bound or is empty")
    require(0 < before[3]["bytes"] <= 128 * 1024 * 1024, "Starting save exceeds 128 MiB bound or is empty")
    summary = read_json(paths[0].read_bytes())
    session_manifest = read_json(paths[1].read_bytes())
    require(type(summary.get("schema")) is int and summary["schema"] == 1 and summary.get("complete") is True
            and summary.get("qualification") == "RECORDED_NOT_REPLAY_VALIDATED"
            and type(summary.get("exit_code")) is int and summary["exit_code"] == 0
            and summary.get("protected_inputs_unchanged") is True and summary.get("errors") == [],
            "Capture is not an intact qualified acquisition")
    session_id = summary.get("session")
    require(isinstance(session_id, str) and re.fullmatch(r"[0-9a-f]{32}", session_id), "Invalid capture session")
    route_name = session_manifest.get("route")
    require(type(session_manifest.get("schema")) is int and session_manifest["schema"] == 1 and session_manifest.get("session") == session_id
            and isinstance(route_name, str) and route_name in ALLOWED_ROUTES, "Session manifest is not a supported selected route recording")
    checked_identity(before[2], summary.get("route_file"), "Route")
    checked_identity(before[3], summary.get("starting_save", {}).get("route_copy"), "Starting save")
    rows = [read_json(line) for line in paths[2].read_bytes().splitlines()]
    require(3 <= len(rows) <= MAX_RECORDS and type(summary.get("record_count")) is int
            and summary["record_count"] == len(rows), "Record count mismatch/budget exceeded")
    counts = Counter()
    for index, row in enumerate(rows, 1):
        require(isinstance(row, dict) and type(row.get("seq")) is int and row["seq"] == index
                and type(row.get("schema")) is int and row["schema"] == 1 and row.get("session") == session_id,
                "Discontinuous sequence or mixed schema/session")
        require(row.get("event") in ALLOWED_EVENTS, "Unsupported action/transition event: " + str(row.get("event")))
        number(row.get("t"), "row wall time", 0, 1e8)
        counts[row["event"]] += 1
    require(counts["ready"] == counts["start"] == counts["finish"] == 1 and rows[0]["event"] == "ready"
            and rows[-1]["event"] == "finish", "Missing/duplicate/reordered route boundary")
    require(dict(counts) == summary.get("events"), "Summary event counts disagree with raw evidence")
    start = next(row for row in rows if row["event"] == "start")
    finish = rows[-1]
    require(start.get("route_name") == finish.get("route_name") == route_name, "Route name mismatch")
    checkpoint_save = checkpoint_authenticated(summary, start, session_id, route_name, before[3])
    require(finish.get("expected_records") == len(rows), "Footer record count mismatch")
    require(finish.get("counts") == {key: value for key, value in counts.items() if key != "finish"}, "Footer event counts disagree")
    flags = finish.get("flags", {})
    require(all(flags.get(key) is True for key in ("complete", "policy_qualified", "policy_ready", "door_observer_ready"))
            and not any(value for key, value in flags.items() if key not in {"complete", "policy_qualified", "policy_ready", "door_observer_ready"}),
            "Footer reports incomplete or unsupported route state")
    policy_qualified(start.get("policy"), "start")
    policy_qualified(finish.get("policy"), "finish")
    source_cohort, cohort_status = movement_cohort(route_name, start, finish, rows[0])
    camera_cohort, camera_cohort_status = camera_layer_cohort(session_manifest, summary, rows[0], start, finish)
    if camera_cohort is not None:
        pose(rows[0].get("pose"), camera_cohort)
        pose(finish.get("pose"), camera_cohort)
    if session_manifest.get("movement_cohort") is not None:
        manifest_cohort = session_manifest["movement_cohort"]
        require(source_cohort is not None and isinstance(manifest_cohort, dict)
                and set(manifest_cohort) == set(NATIVE_MOVEMENT_COHORT)
                and manifest_cohort.get("model") == source_cohort["model"]
                and manifest_cohort.get("private_override") is True, "Session/acquisition movement cohort mismatch")
    require(all(value == 0 for value in finish.get("activation_coverage", {}).values())
            and finish.get("activation_coverage", {}).get("input_attempts") == 0, "Route contains activation attempts")
    duration = number(finish.get("duration_simulation"), "simulation duration", 0.01, MAX_DURATION)
    number(finish.get("duration_wall"), "wall duration", 0.01, MAX_WALL_DURATION)
    require(summary.get("duration_seconds") == finish["duration_wall"], "Summary/footer duration mismatch")
    start_pose = pose(start.get("checkpoint_pose"), camera_cohort)
    pose(start.get("pose"), camera_cohort)
    controls, samples, markers, trigger_audit, frame_times = [], [], [], [], {}
    started = False
    for row in rows:
        event = row["event"]
        if event == "start":
            started = True
            continue
        if event in ("ready", "finish"):
            continue
        require(started, "Work event precedes recording start")
        if event in ("control", "sample", "pause"):
            timestamp = number(row.get("simulation"), "simulation timestamp", 0, duration)
            frame = number(row.get("f"), "frame", 1, 1000000)
            require(type(frame) is int, "Frame must be an integer")
            if frame in frame_times:
                require(frame_times[frame] == timestamp, "Same frame has conflicting simulation timestamps")
            frame_times[frame] = timestamp
        if event == "control":
            require(not controls or timestamp >= controls[-1]["t"], "Nonmonotonic control timeline")
            values = resolved_controls(row.get("controls"))
            actions = row.get("actions", {})
            require(set(actions) <= {"forward", "backward", "left", "right", "run", "sneak", "use"}
                    and actions.get("use") is False, "Unsupported input action")
            controls.append({"t": timestamp, **values})
        elif event == "sample":
            require(row.get("paused") is False, "Paused sample is unsupported")
            require(not samples or timestamp > samples[-1]["t"], "Nonmonotonic sampled timeline")
            samples.append({"t": timestamp, **pose(row, camera_cohort)})
        elif event == "checkpoint":
            pose(row.get("pose"), camera_cohort)
            markers.append({"frame": row.get("f"), "index": row.get("index")})
        elif event == "cell":
            cell(row.get("cell"))
        elif event == "pause":
            require(row.get("paused") is False, "Paused/menu transitions are unsupported")
        elif event == "policy":
            require(row.get("policy_event") not in {"combat", "policy_failed", "suppress_failed"}, "Policy reports combat/failure")
        elif event == "trigger":
            require(isinstance(row.get("name"), str) and row["name"] in SUPPORTED_MOVEMENT_TRIGGERS,
                    "Unsupported input trigger: " + str(row.get("name")))
            trigger_time = number(row.get("simulation"), "trigger simulation timestamp", 0, duration)
            trigger_frame = number(row.get("f"), "trigger frame", 1, 1000000)
            require(type(trigger_frame) is int, "Trigger frame must be an integer")
            require(not trigger_audit or (trigger_time >= trigger_audit[-1]["t"] and trigger_frame >= trigger_audit[-1]["f"]),
                    "Nonmonotonic supported trigger audit")
            # Trigger callbacks and onFrame observations can share a native frame
            # number with different simulation timestamps. Do not merge their phases.
            trigger_audit.append({"name": row["name"], "t": trigger_time, "f": trigger_frame,
                                  "source_sequence": row["seq"], "wall_time": row["t"],
                                  "observation": text(row.get("observation"), "trigger observation"),
                                  "playback_replayed_as": "resolved_native_control_snapshot"})
    require(controls and samples and controls[0]["t"] == samples[0]["t"] == 0, "Timeline lacks start coverage")
    require(duration - samples[-1]["t"] <= 2, "Timeline lacks ending pose coverage")
    frames = sorted(frame_times)
    times = [frame_times[frame] for frame in frames]
    require(all(right >= left for left, right in zip(times, times[1:])), "Frame schedule is not monotonic")
    if trigger_audit:
        require(type(finish.get("frames")) is int and frames[-1] <= finish["frames"] <= 1000000
                and all(frames[0] <= trigger["f"] <= finish["frames"] for trigger in trigger_audit),
                "Supported trigger has no acquisition frame coverage")
    resolved_boundaries = None
    if trigger_audit or start.get("resolved_controls") is not None or finish.get("resolved_controls") is not None:
        initial = resolved_controls(start.get("resolved_controls"))
        final = resolved_controls(finish.get("resolved_controls"))
        require(initial == {key: value for key, value in controls[0].items() if key != "t"},
                "Initial resolved controls disagree with time-zero input")
        terminal_added = final != {key: value for key, value in controls[-1].items() if key != "t"}
        if terminal_added:
            require(duration > controls[-1]["t"], "Conflicting resolved controls at acquisition end")
            controls.append({"t": duration, **final})
        resolved_boundaries = {"initial": initial, "final": final,
                               "source": "START and FINISH acquisition-end resolved controls",
                               "terminal_input_added": terminal_added}
    for marker in markers:
        frame = marker.pop("frame")
        require(type(frame) is int and frames[0] <= frame <= frames[-1], "Marker has no simulation frame coverage")
        require(type(marker["index"]) is int and marker["index"] > 0, "Invalid marker index")
        index = bisect.bisect_left(frames, frame)
        if frames[index] == frame:
            marker["t"] = times[index]
        else:
            fraction = (frame - frames[index - 1]) / (frames[index] - frames[index - 1])
            marker["t"] = times[index - 1] + fraction * (times[index] - times[index - 1])
    require(len({marker["index"] for marker in markers}) == len(markers), "Duplicate marker index")
    timeline = {"schema": 1, "route_name": route_name, "duration": duration, "source_record_count": len(rows),
                "movement_cohort": source_cohort, "movement_cohort_status": cohort_status,
                "camera_layer_cohort": camera_cohort, "camera_layer_cohort_status": camera_cohort_status,
                "start_pose": start_pose, "controls": controls, "samples": samples, "markers": markers,
                "movement_trigger_audit": trigger_audit, "resolved_control_boundaries": resolved_boundaries}
    generated = ("-- Generated qualified route acquisition; preview only, not benchmark acceptance.\nreturn " + lua(timeline) + "\n").encode("utf-8")
    require(before == [identity(path) for path in paths], "Capture changed during compilation")
    output.parent.mkdir(parents=True, exist_ok=True)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(generated)
    after = [identity(path) for path in paths]
    require(before == after, "Capture changed while writing compiled copy")
    result = {"schema": 1, "qualification": "COMPILED_NOT_REPLAY_VALIDATED", "source_session": session_id,
              "route_name": route_name, "duration_simulation": duration, "inputs_before": before, "inputs_after": after,
              "movement_cohort": source_cohort, "movement_cohort_status": cohort_status,
              "camera_layer_cohort": camera_cohort, "camera_layer_cohort_status": camera_cohort_status,
              "checkpoint_save": checkpoint_save,
              "movement_trigger_audit": trigger_audit, "resolved_control_boundaries": resolved_boundaries,
              "source_evidence_unchanged": True, "timeline": identity(output), "source_record_count": len(rows),
              "control_count": len(controls), "source_control_count": counts["control"],
              "sample_count": len(samples), "marker_count": len(markers),
              "schedule": "relative simulation timestamps; wall times are evidence only",
              "scope": "Selected exterior first-person motion preview. Additive camera channels require an explicit recorded cohort and are interpolated from sampled values; this does not prove exact rendered-camera or benchmark equivalence. Source movement cohort is recorded or explicitly legacy/unrecorded. AutoMove/AlwaysRun/ToggleSneak are retained as audit evidence; only resolved movement/gait inputs are replayed. No activations, interior transitions, menus, or performance qualification."}
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--receipt", required=True)
    args = parser.parse_args()
    try:
        result = compile_route(args.capture, args.output, args.receipt)
    except (CompileError, OSError) as exc:
        print("Route compile refused: " + str(exc), file=sys.stderr)
        return 1
    print(json.dumps({key: result[key] for key in ("qualification", "duration_simulation", "source_record_count", "control_count", "sample_count", "marker_count")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
