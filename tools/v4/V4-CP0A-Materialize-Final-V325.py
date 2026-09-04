#!/usr/bin/env python3
"""Materialize and verify the exact generated source used by final V3.25 Mode151.

This is a CP0A provenance/transition tool. It intentionally reuses the frozen
V3.25 generator once, verifies the generated source patch identity, and leaves
the materialized source in the working tree for inspection or a subsequent
explicit commit. V4 runtime/build logic must not depend on this V3 harness.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ROUTER = ROOT / "tools/v3/apply_diagnostic_harness.py"
PATCH = ROOT / "V3-applied-source.patch"
STAT = ROOT / "V3-applied-source-stat.txt"

FINAL_V325_BRANCH = "v3.25-engine-ownership-bridge-cp2-actorbatch"
EXPECTED_PATCH_SHA256 = "ac15332aeff34a5ce6a442e169704aab6f5eb0f1600b914d863b4561452d4c35"
EXPECTED_CANONICAL_PATCH_SHA256 = "a72c6456f2fd575f9c47db296e7bdf8befee016d10ec44c5829028cd15ba1502"
EXPECTED_STAT_SHA256 = "c01f35f1a047b624b14daec279580805722fe4dbacdd5bb2b178e071ca28f045"
EXPECTED_CHANGED_FILES = 103


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def canonical_patch_sha256(path: Path) -> str:
    # Git versions can choose different abbreviation lengths for the object IDs
    # on `index` lines. Those lines do not change the patch hunks or target
    # source. Remove only `index` lines and hash every remaining byte-equivalent
    # line so semantic patch content still has an exact fail-closed identity.
    lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
    canonical = "\n".join(line for line in lines if not line.startswith("index ")) + "\n"
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def patch_changed_files(path: Path) -> list[str]:
    prefix = "diff --git a/"
    files: list[str] = []
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        if not line.startswith(prefix):
            continue
        rest = line[len(prefix) :]
        marker = " b/"
        if marker not in rest:
            raise RuntimeError(f"malformed diff header: {line}")
        files.append(rest.split(marker, 1)[0])
    return files


def main() -> int:
    if not ROUTER.is_file():
        raise RuntimeError(f"missing frozen V3 router: {ROUTER}")

    # Refuse to start on a dirty source tree. The V4 audit files themselves are
    # committed, so any existing modification means we would lose provenance.
    status = subprocess.run(
        ["git", "status", "--porcelain"], cwd=ROOT, check=True, text=True, capture_output=True
    ).stdout
    if status.strip():
        raise RuntimeError("working tree is not clean; materialization refused")

    # The accepted V3.25 preflight artifact was generated with seven-character
    # abbreviated blob IDs in patch `index` lines. Newer Git can auto-select a
    # longer abbreviation. Pin the old display format for byte-for-byte archival
    # reproducibility; the canonical hash below independently verifies all patch
    # content except those presentation-only index lines.
    subprocess.run(["git", "config", "--local", "core.abbrev", "7"], cwd=ROOT, check=True)

    env = os.environ.copy()
    # The V3.25 router selects the actor-batch refinement from this exact branch
    # identity. We deliberately inject the frozen identity even though the V4
    # audit branch has a different name.
    env["GITHUB_REF_NAME"] = FINAL_V325_BRANCH

    subprocess.run([sys.executable, str(ROUTER)], cwd=ROOT, env=env, check=True)

    for required in (PATCH, STAT):
        if not required.is_file():
            raise RuntimeError(f"V3 generator did not produce {required.name}")

    patch_hash = sha256(PATCH)
    canonical_hash = canonical_patch_sha256(PATCH)
    stat_hash = sha256(STAT)
    if canonical_hash != EXPECTED_CANONICAL_PATCH_SHA256:
        raise RuntimeError(
            f"canonical generated patch mismatch: expected {EXPECTED_CANONICAL_PATCH_SHA256}, got {canonical_hash}"
        )
    if patch_hash != EXPECTED_PATCH_SHA256:
        raise RuntimeError(
            f"byte-for-byte generated patch mismatch after core.abbrev=7: expected {EXPECTED_PATCH_SHA256}, got {patch_hash}"
        )
    if stat_hash != EXPECTED_STAT_SHA256:
        raise RuntimeError(
            f"generated stat mismatch: expected {EXPECTED_STAT_SHA256}, got {stat_hash}"
        )

    changed = patch_changed_files(PATCH)
    if len(changed) != EXPECTED_CHANGED_FILES or len(set(changed)) != EXPECTED_CHANGED_FILES:
        raise RuntimeError(
            f"generated patch file-count mismatch: expected {EXPECTED_CHANGED_FILES} unique files, "
            f"got {len(changed)} headers / {len(set(changed))} unique"
        )

    subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

    print("Final V3.25 Mode151 generated source materialized and verified.")
    print(f"patch_sha256={patch_hash}")
    print(f"canonical_patch_sha256={canonical_hash}")
    print(f"stat_sha256={stat_hash}")
    print(f"changed_source_files={len(changed)}")
    print("Next: inspect/reconcile this source and commit it explicitly on the V4 lineage; do not rerun the V3 harness at V4 runtime/build time.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
