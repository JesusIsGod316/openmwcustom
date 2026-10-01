#!/usr/bin/env python3
"""Qualify actual Lua allocator semantics and explicit VM GC evidence."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys


def metrics(row):
    return {row[f"metric{i}_name"]: int(row[f"metric{i}_value"])
            for i in range(1, 5) if row[f"metric{i}_name"]}


def run(executable, output, name, selected, deep=False, fallback=False):
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith(("OPENMW_HITCH_", "OPENMW_P9_HITCH_", "OPENMW_P9_LUA_")):
            environment.pop(key)
    directory = output / name
    directory.mkdir(parents=True, exist_ok=True)
    spans_path = directory / "lua-allocator-gc-spans.csv"
    if deep:
        environment["OPENMW_P9_HITCH_SPANS_FILE"] = str(spans_path)
    if fallback:
        environment["OPENMW_P9_LUA_NATIVE_ALLOCATOR_FIXTURE"] = "1"
    result = subprocess.run([str(executable), f"--gtest_filter={selected}"], cwd=directory,
                            env=environment, capture_output=True, text=True, timeout=60)
    (directory / "components-tests.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"{name} production tests failed: {result.returncode}\n{result.stdout}\n{result.stderr}"
    count = selected.count(":") + 1
    assert f"[  PASSED  ] {count} test" in result.stdout, f"{name}: tests were skipped or missing"
    if not deep:
        assert not spans_path.exists(), "Normal process unexpectedly wrote a Deep capture"
        return []
    status = dict(line.split("=", 1) for line in Path(str(spans_path) + ".capture-status.txt")
                  .read_text(encoding="utf-8").splitlines())
    assert all(status[field] == "0" for field in ("dropped", "allocation_failed", "active_at_finish")), status
    assert status["normal_finish"] == "1" and status["output_ok"] == "1", status
    with spans_path.open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(line for line in source if not line.startswith("#")))
    assert len(rows) == int(status["rows"]) and rows, "Capture row count mismatch"
    by_id = {row["span"]: row for row in rows}
    assert all(row["parent"] == "0" or row["parent"] in by_id for row in rows), "Lost nesting parent"
    for row in rows:
        if row["phase"].startswith(("lua.alloc.", "lua.gc.")):
            assert float(row["thread_cpu_ms"]) == -1, "New evidence queried an OS thread CPU timer"
    observations = [row for row in rows if row["phase"] == "lua.gc.observation"]
    assert observations and all(row["identity"] == "unavailable_without_vm_sampler" for row in observations)
    assert all(metrics(row)["automatic_gc_observed"] == 0 and metrics(row)["vm_sampler_enabled"] == 0
               and metrics(row)["tracking_available"] == int(not fallback) for row in observations)
    services = [row for row in rows if row["phase"] == "lua.alloc.service"]
    assert services, "Missing production allocator operation summary"
    small_names = {"req_le64", "req_65_256", "req_257_1024", "req_1025_16384"}
    large_names = {"req_16385_1048576", "req_gt1048576", "free_requests", "tracking_available"}
    large_rows = [row for row in rows if row["phase"] == "lua.alloc.request_large"]
    assert large_rows, "Missing requested-size histogram"
    for large in large_rows:
        small = by_id[large["parent"]]
        service = by_id[small["parent"]]
        assert small["phase"] == "lua.alloc.request_small" and service["phase"] == "lua.alloc.service"
        assert set(metrics(small)) == small_names and set(metrics(large)) == large_names
        assert metrics(large)["tracking_available"] == int(not fallback)
        counts = sum(metrics(small).values()) + metrics(large)["req_16385_1048576"] + metrics(large)["req_gt1048576"]
        counts += metrics(large)["free_requests"]
        service_counts = metrics(service)
        assert counts == service_counts["service_calls"] + service_counts["policy_blocked"]
        if fallback:
            assert counts == 0 and all(value == 0 for value in service_counts.values())
    if not fallback:
        assert any(metrics(row)["service_ns"] > 0 and metrics(row)["service_calls"] > 0 for row in services)
    steps = [row for row in rows if row["phase"] == "lua.gc.step_metrics"]
    assert steps, "Missing actual explicit VM-step evidence"
    assert all(row["asset"] == "1024" and metrics(row)["requested_steps"] == 1024 for row in steps)
    assert any(metrics(row)["cycle_completed"] == 1 for row in steps), "No VM-returned completed GC cycle"
    assert all(metrics(row)["cycle_completed"] in (0, 1) and metrics(row)["heap_before_bytes"] > 0
               and metrics(row)["heap_after_bytes"] > 0 for row in steps)
    if not fallback:
        assert any(float(row["wall_ms"]) >= 1 for row in steps), "Real finalizer was not exercised inside GC"
    return rows


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    selected = ":".join("LuaStateTest." + name for name in (
        "HitchAllocationCounters", "HitchAllocatorRequestSizesPreserveRealloc",
        "HitchMemoryLimitDenialPreservesPointer", "HitchGcStepCompletesCycleAndFinalizers"))
    run(executable, output, "normal", selected)
    tracked = run(executable, output, "deep", selected, deep=True)
    native = run(executable, output, "native-fallback", "LuaDiagnosticsFallback.NativeAllocatorPreservesGcAndSemantics",
                 deep=True, fallback=True)
    report = {"status": "passed", "processes": 3, "normal_tests": 4, "deep_tests": 4,
              "native_fallback_tests": 1, "tracked_rows": len(tracked), "native_rows": len(native),
              "capture_complete": True, "allocator_service_unit": "nanoseconds",
              "requested_size_unit": "attempt counts of logical nsize bytes",
              "gc_completion_source": "LUA_GCSTEP returned 1",
              "automatic_gc_observation": "unavailable_without_vm_sampler",
              "scope": "Real Lua allocator semantics and explicit GC evidence; no gameplay performance claim"}
    (output / "LUA-ALLOCATOR-GC-VALIDATION.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
