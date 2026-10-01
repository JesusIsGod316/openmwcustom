#!/usr/bin/env python3
"""Qualify production Lua event/timer/load scopes using existing component tests."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("OPENMW_HITCH_") or name.startswith("OPENMW_P9_HITCH_"):
            environment.pop(name)
    spans_path = output / "lua-container-spans.csv"
    environment["OPENMW_P9_HITCH_SPANS_FILE"] = str(spans_path)
    selected = "LuaScriptsContainerTest.CallEvent:LuaScriptsContainerTest.Timers:LuaScriptsContainerTest.LoadSave:LuaScriptsContainerTest.CallHandler:LuaScriptsContainerTrace.DeepEventTimerAttributionPreservesDelivery"
    result = subprocess.run([str(executable), f"--gtest_filter={selected}"], cwd=output,
                            env=environment, capture_output=True, text=True, timeout=45)
    (output / "components-tests.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"Production Lua tests failed: {result.returncode}\n{result.stdout}\n{result.stderr}"
    assert "[  PASSED  ] 5 tests." in result.stdout, "Expected Lua component tests were not all executed"
    status = dict(line.split("=", 1) for line in Path(str(spans_path) + ".capture-status.txt").read_text(encoding="utf-8").splitlines())
    for field in ("dropped", "allocation_failed", "active_at_finish"):
        assert status[field] == "0", f"Incomplete Lua capture: {field}={status[field]}"
    assert status["normal_finish"] == "1" and status["output_ok"] == "1"
    with spans_path.open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(line for line in source if not line.startswith("#")))
    assert len(rows) == int(status["rows"]) and rows
    by_id = {row["span"]: row for row in rows}
    assert all(row["parent"] == "0" or row["parent"] in by_id for row in rows), "Lost nesting parent"
    for row in rows:
        if row["phase"].startswith(("lua.event.", "lua.timer.", "lua.load.", "lua.script.")):
            assert float(row["thread_cpu_ms"]) == -1, "New Lua scope queried an OS CPU timer"
    handlers = [row for row in rows if row["phase"] == "lua.event.handler" and row["asset"] == "trace-delivery.lua"]
    assert len(handlers) == 1 and handlers[0]["identity"] == "TraceEvent"
    # receiveEvent retains its existing V33 dispatch callback envelope between
    # the new container dispatch and each handler. Qualify that exact chain.
    callback = by_id[handlers[0]["parent"]]
    assert callback["phase"] == "lua.callback" and callback["identity"] == "TraceEvent" and callback["asset"] == ""
    dispatch = by_id[callback["parent"]]
    assert dispatch["phase"] == "lua.event.dispatch" and dispatch["identity"] == "TraceFixture" and dispatch["asset"] == "TraceEvent"
    deliveries = [row for row in rows if row["phase"] == "lua.timer.deliver" and row["asset"] == "trace-delivery.lua"]
    assert len(deliveries) == 2, "Production timers were not individually attributed"
    observed = {}
    for row in deliveries:
        assert row["identity"] == "TraceTimer"
        metrics = {row[f"metric{i}_name"]: int(row[f"metric{i}_value"]) for i in range(1, 5)}
        assert metrics["queue_length"] == 1 and metrics["serializable"] == 1
        parent = by_id[row["parent"]]
        assert parent["phase"] == "lua.timer.queue" and parent["identity"] == "TraceFixture"
        observed[parent["asset"]] = metrics["lateness_us"]
    assert observed == {"simulation": 3_000_000, "game": 10_000_000}, observed
    assert any(row["phase"] == "lua.script.sandbox" for row in rows), "Script startup was not attributed"
    report = {"status": "passed", "production_tests": 5, "capture_complete": True,
              "timer_lateness_us_by_domain": observed,
              "scope": "Production Lua container delivery/load semantics and Deep attribution; no game performance claim"}
    (output / "LUA-CONTAINER-ATTRIBUTION-VALIDATION.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
