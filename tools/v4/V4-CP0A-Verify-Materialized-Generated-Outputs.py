#!/usr/bin/env python3
"""Verify generated V3 outputs materialized into the V4 CP0A source lineage.

The frozen V3.25 harness produced generated source/provenance outputs that were
not represented by the original 103-path source patch because they were
untracked. This validator makes both the six compile/source omissions and the
four legacy packaging/provenance outputs first-class V4 inputs and fails before
an expensive build if any are missing, untracked, or have drifted.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tools/v4/V4-CP0A-GENERATED-OUTPUT-MANIFEST.txt"
PACKAGING_MANIFEST = ROOT / "tools/v4/V4-CP0A-GENERATED-PACKAGING-MANIFEST.txt"

EXPECTED_BASE = "0c47f593c9c464ef158bade8604bfda536a6d628"
EXPECTED_FROZEN_V325 = "f7557829bcb14e339410cefb32b6612e5009e46d"
EXPECTED_PATCH_PATH_COUNT = 103
EXPECTED = {
    "V3.16-HITCH-LAYER.txt": "8dadeae8c25896e6c42f615f2e1eceffda8587e9b10863c34fbf520dfa9d3b0a",
    "V3.17-RUNTIME-LAYER.txt": "100ee2b424d8bdc9053b0e35d5ed00b2add420270e11d7e9f7c0c211713a305e",
    "V3.18-NIS-PROVENANCE.txt": "eef4f8aefe16b89b671334df7b952b9ddbd35696976d0a8477afb669c83c70e6",
    "V3.18-RENDER-LAYER.txt": "60fc90c44914db5178b9c334d0106b037568b44f7656ff15fc68a36209704d10",
    "components/debug/v320luafastpath.hpp": "33a2a4c3b29939ec235ade23acf75cbbe7f098413715860db1d5095e04513d99",
    "components/resource/v321classifiedcompileset.hpp": "a0959b688f283286c46e44fc28e1bc20dec5b5ff7c0d5443728217fffa390a2a",
}
EXPECTED_PACKAGING = {
    "V3-applied-source.patch": "ac15332aeff34a5ce6a442e169704aab6f5eb0f1600b914d863b4561452d4c35",
    "V3-applied-source-stat.txt": "c01f35f1a047b624b14daec279580805722fe4dbacdd5bb2b178e071ca28f045",
    "V3-BUILD-IDENTITY.txt": "3fd1e9c9410bba493bdb9e30f6bfc92382d9c579a911a277565abc8bd73f0bfc",
    "V3-LAB-README.txt": "47c6cd8bf004cd6551efd59788fe84a668fb5fa8ed978c4d54e2ca46612472e5",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_manifest() -> dict[str, str]:
    if not MANIFEST.is_file():
        raise RuntimeError(f"missing generated-output manifest: {MANIFEST.relative_to(ROOT)}")

    lines = MANIFEST.read_text(encoding="utf-8", errors="strict").splitlines()
    required_header = [
        "V4_CP0A_GENERATED_OUTPUT_MANIFEST/1",
        f"BASE={EXPECTED_BASE}",
        f"FROZEN_V325_SOURCE={EXPECTED_FROZEN_V325}",
        f"PATCH_PATH_COUNT={EXPECTED_PATCH_PATH_COUNT}",
        f"AUDITED_OMISSION_COUNT={len(EXPECTED)}",
        "",
    ]
    if lines[: len(required_header)] != required_header:
        raise RuntimeError("generated-output manifest identity/header drifted")

    parsed: dict[str, str] = {}
    for line in lines[len(required_header) :]:
        if not line:
            continue
        path, marker, value = line.partition("|sha256=")
        if not marker or not path or not value:
            raise RuntimeError(f"malformed generated-output manifest line: {line!r}")
        if path in parsed:
            raise RuntimeError(f"duplicate generated-output manifest path: {path}")
        parsed[path] = value

    if parsed != EXPECTED:
        missing = sorted(set(EXPECTED) - set(parsed))
        extra = sorted(set(parsed) - set(EXPECTED))
        mismatched = sorted(path for path in set(EXPECTED) & set(parsed) if EXPECTED[path] != parsed[path])
        raise RuntimeError(
            "generated-output manifest does not match the frozen audit: "
            f"missing={missing}, extra={extra}, hash_mismatch={mismatched}"
        )
    return parsed


def parse_packaging_manifest() -> dict[str, str]:
    if not PACKAGING_MANIFEST.is_file():
        raise RuntimeError(f"missing generated-packaging manifest: {PACKAGING_MANIFEST.relative_to(ROOT)}")

    lines = PACKAGING_MANIFEST.read_text(encoding="utf-8", errors="strict").splitlines()
    required_header = [
        "V4_CP0A_GENERATED_PACKAGING_MANIFEST/1",
        f"BASE={EXPECTED_BASE}",
        f"FROZEN_V325_SOURCE={EXPECTED_FROZEN_V325}",
        f"PACKAGING_OUTPUT_COUNT={len(EXPECTED_PACKAGING)}",
        "",
    ]
    if lines[: len(required_header)] != required_header:
        raise RuntimeError("generated-packaging manifest identity/header drifted")

    parsed: dict[str, str] = {}
    for line in lines[len(required_header) :]:
        if not line:
            continue
        path, marker, value = line.partition("|sha256=")
        if not marker or not path or not value:
            raise RuntimeError(f"malformed generated-packaging manifest line: {line!r}")
        if path in parsed:
            raise RuntimeError(f"duplicate generated-packaging manifest path: {path}")
        parsed[path] = value

    if parsed != EXPECTED_PACKAGING:
        missing = sorted(set(EXPECTED_PACKAGING) - set(parsed))
        extra = sorted(set(parsed) - set(EXPECTED_PACKAGING))
        mismatched = sorted(
            path
            for path in set(EXPECTED_PACKAGING) & set(parsed)
            if EXPECTED_PACKAGING[path] != parsed[path]
        )
        raise RuntimeError(
            "generated-packaging manifest does not match the frozen audit: "
            f"missing={missing}, extra={extra}, hash_mismatch={mismatched}"
        )
    return parsed


def verify_payload(expected: dict[str, str], label: str) -> None:
    for rel, digest in expected.items():
        path = ROOT / rel
        if not path.is_file():
            raise RuntimeError(f"{label} is missing: {rel}")
        actual = sha256(path)
        if actual != digest:
            raise RuntimeError(f"{label} hash mismatch for {rel}: expected {digest}, got {actual}")
        subprocess.run(
            ["git", "ls-files", "--error-unmatch", "--", rel],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
        )


def verify_v3_component_includes_resolve() -> None:
    """Catch another generated-header omission before compilation.

    The original defect left tracked V3 source referring to untracked generated
    headers. Scan tracked C/C++ source for repository-local V3 component includes
    and require every referenced path to exist in the materialized tree.
    """

    tracked = subprocess.run(
        ["git", "ls-files", "*.cpp", "*.cxx", "*.cc", "*.hpp", "*.hxx", "*.h"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    include_re = re.compile(r'^\s*#\s*include\s*[<"](?P<path>components/[^>"]*/v3[^>"]*\.(?:hpp|h))[>"]')
    missing: dict[str, list[str]] = {}
    for rel in tracked:
        source = ROOT / rel
        try:
            text = source.read_text(encoding="utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        for line in text.splitlines():
            match = include_re.match(line)
            if not match:
                continue
            target = match.group("path")
            if not (ROOT / target).is_file():
                missing.setdefault(target, []).append(rel)

    if missing:
        detail = "; ".join(f"{target} <- {sorted(users)}" for target, users in sorted(missing.items()))
        raise RuntimeError(f"tracked source contains unresolved V3 component includes: {detail}")


def main() -> int:
    expected = parse_manifest()
    expected_packaging = parse_packaging_manifest()
    verify_payload(expected, "materialized generated output")
    verify_payload(expected_packaging, "materialized generated packaging output")
    verify_v3_component_includes_resolve()
    print(
        "V4 CP0A generated-output materialization verification passed: "
        f"{len(expected)} audited omissions and {len(expected_packaging)} packaging/provenance outputs "
        "are present, tracked, exact-hash, and V3 component includes resolve."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
