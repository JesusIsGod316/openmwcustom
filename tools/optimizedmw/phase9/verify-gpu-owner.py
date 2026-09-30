"""Run the production GL query fixture and inspect its shutdown-flushed rows."""
import csv
import os
from pathlib import Path
import subprocess
import sys

path = Path(sys.argv[2])
env = dict(os.environ, OPENMW_V36_GPU_PASS_FILE=str(path))
subprocess.run([sys.argv[1]], env=env, check=True, timeout=35)
with path.open(encoding="utf-8-sig") as stream:
    rows = list(csv.DictReader(line for line in stream if not line.startswith("#")))
assert rows, "Actual GPU queries never produced an available result"
for row in rows:
    assert 100 <= int(row["source_frame"]) < 140, row
    assert int(row["report_frame"]) >= int(row["source_frame"]), row
    assert int(row["camera"]) != 0, row
    assert int(row["dropped_total"]) >= 0 and int(row["writer_dropped_total"]) >= 0, row
print(f"PASS {len(rows)} actual query rows use draw-owner frames/context/camera; writer loss is separate")
