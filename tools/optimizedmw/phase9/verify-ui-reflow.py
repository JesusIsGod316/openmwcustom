#!/usr/bin/env python3
"""Compare real MyGUI/LuaUi control and candidate in isolated processes."""
import json
import csv
import os
from pathlib import Path
import subprocess
import sys


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = {}
    for label, setting, expected in (
        ("control", "0", "0"),
        ("candidate", "1", "1"),
        ("malformed", "true", "0"),
        ("missing", None, "0"),
        ("candidate_deep", "1", "1"),
    ):
        environment = os.environ.copy()
        # Isolate this production UI mechanism from the other candidate gates.
        for name in tuple(environment):
            if name.startswith("OPENMW_HITCH_") or name.startswith("OPENMW_P9_HITCH_"):
                environment.pop(name)
        if setting is not None:
            environment["OPENMW_HITCH_UI_REFLOW"] = setting
        if label == "candidate_deep":
            environment["OPENMW_P9_HITCH_SPANS_FILE"] = str(output / "candidate-deep-spans.csv")
        destination = output / f"{label}.json"
        completed = subprocess.run(
            [str(executable), str(destination), expected],
            cwd=output, env=environment, check=False, capture_output=True, text=True, timeout=45,
        )
        (output / f"{label}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"{label} production UI fixture failed: {completed.returncode}\n{completed.stdout}\n{completed.stderr}")
        results[label] = json.loads(destination.read_text(encoding="utf-8"))
        assert results[label]["policy_enabled"] == (expected == "1")
        assert results[label]["snapshot_count"] >= 13

    baseline = results["control"]
    for label in ("candidate", "malformed", "missing"):
        assert results[label]["snapshots"] == baseline["snapshots"], f"{label} changed production layout/edit/event output"
    assert baseline["stable_glyph_visits"] > 0, "Control did not exercise repeated real text measurement"
    assert results["candidate"]["stable_glyph_visits"] < baseline["stable_glyph_visits"], "Candidate did not reduce real MyGUI glyph measurement work"
    for label in ("malformed", "missing"):
        assert results[label]["stable_glyph_visits"] == baseline["stable_glyph_visits"], f"{label} changed control mechanism"
    assert results["candidate_deep"]["snapshots"] == results["candidate"]["snapshots"], "Deep instrumentation changed production UI output"
    assert results["candidate_deep"]["stable_glyph_visits"] == results["candidate"]["stable_glyph_visits"], "Deep instrumentation changed measurement reuse"
    spans_path = output / "candidate-deep-spans.csv"
    status_path = Path(str(spans_path) + ".capture-status.txt")
    status = dict(line.split("=", 1) for line in status_path.read_text(encoding="utf-8").splitlines())
    for name in ("dropped", "allocation_failed", "active_at_finish"):
        assert status[name] == "0", f"Incomplete UI Deep capture: {name}={status[name]}"
    assert status["normal_finish"] == "1" and status["output_ok"] == "1", "UI Deep capture did not finish cleanly"
    with spans_path.open(encoding="utf-8", newline="") as source:
        spans = list(csv.DictReader(line for line in source if not line.startswith("#")))
    assert int(status["rows"]) == len(spans) and spans, "UI Deep capture has no attributed rows"
    ui_spans = [row for row in spans if row["phase"].startswith("lua.ui.")]
    assert ui_spans, "New UI subspans did not appear in the production fixture"
    for row in ui_spans:
        assert float(row["thread_cpu_ms"]) == -1, "UI subspan unexpectedly queried an OS CPU timer"
        assert int(row["end_ns"]) >= int(row["start_ns"]), "UI span has reversed timing"
    span_ids = {row["span"] for row in spans}
    assert all(row["parent"] == "0" or row["parent"] in span_ids for row in spans), "UI Deep nesting parent was lost"
    phases = sorted({row["phase"] for row in ui_spans})
    report = {
        "status": "passed",
        "production_geometry_edit_event_equivalence": True,
        "startup_policy_frozen": True,
        "control_stable_glyph_visits": baseline["stable_glyph_visits"],
        "candidate_stable_glyph_visits": results["candidate"]["stable_glyph_visits"],
        "deep_geometry_edit_event_equivalence": True,
        "deep_capture_complete": True,
        "deep_ui_phases": phases,
        "scope": "Real MyGUI/LuaUi CPU layout and editing, headless device and synthetic font atlas; not in-game visual/performance acceptance",
    }
    (output / "UI-REFLOW-VALIDATION.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
