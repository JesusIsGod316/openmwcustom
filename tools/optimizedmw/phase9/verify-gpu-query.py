"""CPU-only production-query dispatch contract; no game/WPR/UAC/native render."""
import csv
import os
from pathlib import Path
import subprocess
import sys

exe = sys.argv[1]
root = Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)
records = {}
for name, mode, defer in [("normal", "normal", False), ("legacy", "available", False),
                         ("deferred", "available", True), ("unavailable-control", "unavailable", False),
                         ("unavailable", "unavailable", True), ("reversed", "reversed", True),
                         ("unjoined", "unjoined", True)]:
    path = root / (name + ".csv")
    env = {k: v for k, v in os.environ.items() if not k.startswith("OPENMW_")}
    env.update(OPENMW_V36_GPU_PASS_FILE="" if mode == "normal" else str(path),
               OPENMW_P9_CAPTURE_TRANSPORT="1", OPENMW_P9_GPU_QUERY_DEFER=str(int(defer)))
    run = subprocess.run([exe, mode], env=env, check=True, timeout=25, text=True, capture_output=True)
    (root / (name + ".log")).write_text(run.stdout + run.stderr)
    if mode == "normal":
        assert not path.exists() and not Path(str(path) + ".query-status.txt").exists()
        continue
    status = dict(line.split("=", 1) for line in Path(str(path) + ".query-status.txt").read_text().splitlines())
    numbers = {k: int(v) for k, v in status.items() if v.isdigit()}
    assert numbers["status_complete"] == numbers["normal_finish"] == numbers["output_ok"] == 1
    assert numbers["issued"] == numbers["collected"] + numbers["pending"] + numbers["active"] + numbers["invalid"]
    assert numbers["active"] == numbers["registration_failures"] == numbers["writer_dropped"] == 0
    assert numbers["accounting_complete"] == (mode != "unjoined")
    assert numbers["query_result_coverage_complete"] == 0  # final2frames honestly remain pending
    with path.open(encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(line for line in stream if not line.startswith("#")))
    assert len(rows) == numbers["collected"]
    with Path(str(path) + ".pending.csv").open() as stream:
        pending = list(csv.DictReader(stream))
    assert len(pending) == numbers["pending"] + numbers["active"]
    assert all(r["status"] == "pending" and int(r["begin_query"]) > 0 and int(r["end_query"]) > 0 for r in pending)
    assert all(int(row["report_frame"]) >= int(row["source_frame"]) + 2 for row in rows)
    if mode == "unavailable":
        assert numbers["issued"] == numbers["pending"] == 16
        assert numbers["admission_dropped"] == 64 and not rows
        assert sum(int(row["unavailable_polls"]) for row in pending) == numbers["unavailable_polls"], \
            "Per-slot unavailable attempt counts were capped or lost"
    if mode == "reversed":
        assert numbers["invalid"] == 76 and numbers["pending"] == 4 and not rows
    if mode == "available":
        assert numbers["issued"] == 80 and numbers["collected"] == 76 and numbers["pending"] == 4
        assert numbers["admission_dropped"] == 0
    records[name] = (numbers, rows)
legacy, deferred = records["legacy"], records["deferred"]
assert [(r["source_frame"], r["gpu_ms"]) for r in legacy[1]] == [(r["source_frame"], r["gpu_ms"]) for r in deferred[1]]
assert deferred[0]["unavailable_polls"] < legacy[0]["unavailable_polls"]
assert records["unavailable"][0]["unavailable_polls"] < records["unavailable-control"][0]["unavailable_polls"]
print("PASS production query contracts: Normal0calls/files; exact same available results; min2/oncebegin/backoff; "
      "bounded-ring drop, reversed timestamps, final pending and unjoined qualification remain explicit")
