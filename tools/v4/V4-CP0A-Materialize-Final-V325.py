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
EXPECTED_STAT_SHA256 = "c01f35f1a047b624b14daec279580805722fe4dbacdd5bb2b178e071ca28f045"
EXPECTED_CHANGED_FILES = 103


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


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
    stat_hash = sha256(STAT)
    if patch_hash != EXPECTED_PATCH_SHA256:
        raise RuntimeError(
            f"generated patch mismatch: expected {EXPECTED_PATCH_SHA256}, got {patch_hash}"
        )
    if stat_hash != EXPECTED_STAT_SHA256:
        raise RuntimeError(
            f"generated stat mismatch: expected {EXPECTED_STAT_SHA256}, got {stat_hash}"
        )

    changed = subprocess.run(
        ["git", "diff", "--name-only"], cwd=ROOT, check=True, text=True, capture_output=True
    ).stdout.splitlines()
    # Generated provenance files may be untracked and therefore are not counted
    # here. The authoritative source-stat says 103 changed tracked files.
    if len(changed) != EXPECTED_CHANGED_FILES:
        raise RuntimeError(
            f"tracked generated-file count mismatch: expected {EXPECTED_CHANGED_FILES}, got {len(changed)}"
        )

    subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

    print("Final V3.25 Mode151 generated source materialized and verified.")
    print(f"patch_sha256={patch_hash}")
    print(f"stat_sha256={stat_hash}")
    print(f"changed_tracked_files={len(changed)}")
    print("Next: inspect/reconcile this source and commit it explicitly on the V4 lineage; do not rerun the V3 harness at V4 runtime/build time.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
