#!/usr/bin/env python3
"""Convert OPENMW_V3_TRACE_FILE CSV output to Chrome/Perfetto trace JSON.

The V3 trace records scope completion time plus duration. This tool reconstructs
scope start timestamps and emits complete ('X') events. It intentionally uses
only the Python standard library so a captured profile remains portable.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def convert(source: Path, destination: Path) -> None:
    events: list[dict] = []
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        required = {
            "frame",
            "epoch_ms",
            "thread",
            "id",
            "parent",
            "category",
            "name",
            "detail",
            "duration_ms",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            missing = sorted(required.difference(reader.fieldnames or []))
            raise SystemExit(f"Not a V3 trace CSV; missing columns: {', '.join(missing)}")

        for row in reader:
            try:
                end_ms = float(row["epoch_ms"])
                duration_ms = float(row["duration_ms"])
                tid = int(row["thread"])
                frame = int(row["frame"])
                event_id = int(row["id"])
                parent = int(row["parent"])
            except (TypeError, ValueError):
                continue

            start_us = (end_ms - duration_ms) * 1000.0
            events.append(
                {
                    "name": row["name"],
                    "cat": row["category"],
                    "ph": "X",
                    "ts": start_us,
                    "dur": duration_ms * 1000.0,
                    "pid": 1,
                    "tid": tid,
                    "args": {
                        "frame": frame,
                        "detail": row["detail"],
                        "v3_id": event_id,
                        "v3_parent": parent,
                    },
                }
            )

    destination.write_text(json.dumps({"traceEvents": events}, separators=(",", ":")), encoding="utf-8")
    print(f"Wrote {len(events)} events to {destination}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace_csv", type=Path)
    parser.add_argument("output_json", type=Path, nargs="?")
    args = parser.parse_args()
    output = args.output_json or args.trace_csv.with_suffix(".trace.json")
    convert(args.trace_csv, output)


if __name__ == "__main__":
    main()
