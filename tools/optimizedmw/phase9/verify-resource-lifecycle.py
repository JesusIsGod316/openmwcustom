#!/usr/bin/env python3
"""Validate actual cache lifetime/concurrency and model contracts in two processes."""
import csv
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def metrics(row):
    return {row[f"metric{i}_name"]: int(row[f"metric{i}_value"])
            for i in range(1, 5) if row[f"metric{i}_name"]}


def run(executable, output, deep):
    directory = output / ("deep" if deep else "normal")
    directory.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith(("OPENMW_HITCH_", "OPENMW_P9_HITCH_")):
            environment.pop(key)
    path = directory / "p9-hitch-spans.csv"
    if deep:
        environment["OPENMW_P9_HITCH_SPANS_FILE"] = str(path)
        environment["OPENMW_P9_HITCH_TRANSPORT"] = "stream"
    selected = "ResourceLifecycleRegistry.*:ResourceLifecycleProduction.*:ResourceLifecycleFinalStatus.*"
    result = subprocess.run([str(executable), f"--gtest_filter={selected}"], cwd=directory,
                            env=environment, capture_output=True, text=True, timeout=90)
    (directory / "components-tests.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"Production resource tests failed: {result.returncode}\n{result.stdout}\n{result.stderr}"
    count = re.search(r"Running (\d+) tests from", result.stdout)
    passed = re.search(r"\[  PASSED  \] (\d+) tests?\.", result.stdout)
    assert count and int(count[1]) >= 12 and passed, "Missing production fixture tests"
    if not deep:
        assert not path.exists(), "Normal unexpectedly wrote a Deep timeline"
        assert not Path(str(path) + ".lifecycle-status.txt").exists(), "Normal unexpectedly wrote lifecycle status"
        assert int(passed[1]) >= 7, "Normal model/cache equivalence contracts did not run"
        return {"tests": int(count[1]), "passed": int(passed[1]), "deep": False}
    assert int(passed[1]) == int(count[1]), "Deep policy tests were skipped"
    status = dict(line.split("=", 1) for line in Path(str(path) + ".capture-status.txt")
                  .read_text(encoding="utf-8").splitlines())
    assert all(status[key] == "0" for key in ("dropped", "allocation_failed", "active_at_finish")), status
    assert status["normal_finish"] == status["output_ok"] == "1", status
    with path.open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(line for line in source if not line.startswith("#")))
    assert rows and len(rows) == int(status["rows"])
    nodes = {int(row["span"]): row for row in rows}
    assert len(nodes) == len(rows), "Duplicate retained identity"
    for row in rows:
        parent, link = int(row["parent"]), int(row["link"])
        if parent:
            p = nodes[parent]
            assert p["thread"] == row["thread"] and int(p["start_ns"]) <= int(row["start_ns"]) <= int(row["end_ns"]) <= int(p["end_ns"])
        assert not link or link in nodes and link < int(row["span"]), "Lost exact causal source"
    publications = {metrics(row)["lifecycle_id"]: row for row in rows if row["phase"] == "resource.lifecycle_publish"}
    assert len(publications) >= 7, "Real cache publication fixtures did not emit lifetimes"
    retired, first = set(), set()
    for row in rows:
        if row["phase"] not in ("resource.lifecycle_first_demand", "resource.lifecycle_retire", "resource.lifecycle_prepare_hit"):
            continue
        m = metrics(row)
        publication = publications[m["lifecycle_id"]]
        assert row["identity"] == publication["identity"] and row["asset"] == publication["asset"]
        if row["phase"] == "resource.lifecycle_retire":
            assert m["lifecycle_id"] not in retired and row["link"] == publication["span"]
            retired.add(m["lifecycle_id"])
        elif row["phase"] == "resource.lifecycle_first_demand":
            assert m["lifecycle_id"] not in first and row["link"] == publication["span"]
            assert m["resource_ptr"] == metrics(publication)["resource_ptr"]
            first.add(m["lifecycle_id"])
    statuses = [row for row in rows if row["phase"] == "resource.lifecycle_status"]
    coverage = [row for row in rows if row["phase"] == "resource.lifecycle_coverage"]
    assert len(statuses) == len(coverage) == 1
    s, c = metrics(statuses[0]), metrics(coverage[0])
    final = dict(line.split("=", 1) for line in Path(str(path) + ".lifecycle-status.txt")
                 .read_text(encoding="utf-8").splitlines())
    assert final["schema"] == final["normal_finish"] == final["output_ok"] == "1"
    assert final["status_span_id"] == statuses[0]["span"] and final["coverage_span_id"] == coverage[0]["span"]
    assert all(int(final[key]) == value for key, value in {**s, **c}.items())
    assert int(final["first_demands"]) == len(first) and int(final["retirements"]) == len(retired)
    assert s["publications"] == len(publications) == len(retired) and c["active"] == 0
    assert s["capacity_failures"] == s["allocation_failures"] == s["metadata_truncations"] == 0
    assert len(first) >= 2
    return {"tests": int(count[1]), "passed": int(passed[1]), "deep": True,
            "rows": len(rows), "lifetimes": len(publications), "first_api_retrievals": len(first),
            "retirements": len(retired), "unknown_origins": c["unknown_origins"],
            "scope": "Real cache ownership/concurrency and synthetic NIF/clone equivalence; no gameplay performance claim"}


if __name__ == "__main__":
    executable, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = [run(executable, output, deep) for deep in (False, True)]
    (output / "RESOURCE-LIFECYCLE-VALIDATION.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS resource lifetimes: production cache ownership/concurrency, exact publication/retrieval/retirement joins, pointer reuse/failure policies, model/clone equivalence and Normal silence")
