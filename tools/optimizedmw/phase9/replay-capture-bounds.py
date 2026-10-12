#!/usr/bin/env python3
"""Read retained captures without modifying them; explain bounded catalog sizing.

This does not recover discarded descriptors or certify runtime performance.
Every dropped insertion attempt is treated as unique for a conservative upper
bound. Resource StateSet/camera joins are retained rather than collapsed.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    modes = [
        "REFERENCE_20260929_230209_731_bc86d0",
        "TEMPORAL-INPUTS_20260929_230535_979_167d0b",
        "ROOT-CAUSE-TRACE_20260929_232743_528_4e62d7",
        "TEMPORAL-TRACE_20260929_233039_640_342375",
        "MOTION-VIEW_20260929_233748_543_67b58c",
        "TEMPORAL-OWNERSHIP_20260929_233851_331_db3289",
        "COMPOSITE-PREPARE_20260929_234147_494_93cf49",
    ]
    result = {"schema": 1, "default_catalog_capacity": 131072, "maximum_catalog_capacity": 262144,
              "scope": "raw read-only cardinality and independent recorded integrity; no runtime promotion", "runs": []}
    for mode in modes:
        profile = args.profiles_root / ("OptimizedMW_Phase9_" + mode)
        run = {"mode": mode, "profile": str(profile), "input_sha256": {}, "writer_losses": {}}
        for path in profile.glob("*.csv"):
            # The transport's final drop counter can be read in bounded space.
            with path.open("rb") as source:
                source.seek(max(0, path.stat().st_size - 4096))
                tail = source.read().decode("utf-8", errors="replace")
            for line in tail.splitlines():
                if line.startswith("# v3_async_diagnostics_dropped_lines="):
                    run["writer_losses"][path.name] = int(line.split("=", 1)[1])
        for name in ("v3-frame.csv.capture-status.txt", "p9-draw-phases.csv.status.txt"):
            path = profile / name
            if not path.exists():
                continue
            run["input_sha256"][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            fields = dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
            if name.startswith("v3-frame"):
                run["cpu_frame_status"] = fields
                run["cpu_frame_complete"] = all(fields.get(key) == value for key, value in {
                    "dropped": "0", "allocation_failed": "0", "normal_finish": "1", "frame_output_ok": "1"}.items())
            else:
                catalog = profile / "p9-draw-phases.csv.resources.csv"
                run["input_sha256"][catalog.name] = hashlib.sha256(catalog.read_bytes()).hexdigest()
                with catalog.open(encoding="utf-8-sig", newline="") as source:
                    rows = list(csv.DictReader(source))
                attempts = int(fields["resource_catalog_dropped_attempts"])
                upper = len(rows) + attempts
                keys = ("context", "texture_unit", "texture", "image", "image_revision", "stateset", "submit_camera",
                        "bytes", "scope", "texture_class", "filename", "semantic_role")
                run["catalog"] = {"retained_rows": len(rows), "dropped_attempts": attempts,
                    "unseen_unique_upper_bound": upper, "fits_default_bound": upper <= 131072,
                    "unique_tuple_rows": len({tuple(row[k] for k in keys) for row in rows}),
                    "unique_columns": {key: len({row[key] for row in rows}) for key in keys},
                    "scope_counts": dict(collections.Counter(row["scope"] for row in rows)),
                    "collapse_without_stateset_rows": len({tuple(row[k] for k in keys if k != "stateset") for row in rows}),
                    "collapse_without_camera_rows": len({tuple(row[k] for k in keys if k != "submit_camera") for row in rows}),
                    "warning": "Collapse alternatives erase joins; dropped attempts are not a count of unique missing resources."}
                if not run["catalog"]["fits_default_bound"]:
                    raise SystemExit("Supplied capture conservative bound does not fit selected default: " + mode)
        result["runs"].append(run)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("PASS: seven raw profiles read without modification; both trace conservative upper bounds fit131072; "
          "CPU status and transport losses remain independent. " + str(args.output))


if __name__ == "__main__":
    main()
