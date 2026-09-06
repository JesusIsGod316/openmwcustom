#!/usr/bin/env python3
"""OpenMW V4 CP3B4 manifest-driven real-NIF conformance runner.

The runner deliberately knows nothing about proprietary game install locations.
It consumes user-supplied VFS data roots/archives and a portable manifest of VFS
NIF paths, invokes the CP3B3 real-NIF tool once or repeatedly per asset, and
produces one deterministic machine-readable corpus report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

CORPUS_SCHEMA = "openmw-v4-cp3b4-corpus-v1"
ASSET_REPORT_SCHEMA = "openmw-v4-cp3b4-asset-report-v1"
CORPUS_REPORT_SCHEMA = "openmw-v4-cp3b4-corpus-report-v1"
SUPPORTED_ENCODINGS = ("win1250", "win1251", "win1252")

DEFAULT_EXPECT = {
    "complete": True,
    "minMeaningful": 1,
    "maxDeferred": 0,
    "maxUnsupported": 0,
    "maxUnsupportedTextureBindings": 0,
}

INTEGER_EXPECTATIONS = {
    "minRendered": ("translation", "rendered", "min"),
    "minCollisionOnly": ("translation", "collisionOnly", "min"),
    "minHidden": ("translation", "hidden", "min"),
    "minDraws": ("realization", "draws", "min"),
    "maxDeferred": ("translation", "deferred", "max"),
    "maxUnsupported": ("translation", "unsupported", "max"),
    "maxIgnored": ("translation", "ignored", "max"),
    "maxUnsupportedTextureBindings": ("realization", "unsupportedTextureBindings", "max"),
}

TRANSLATION_FIELDS = ("rendered", "collisionOnly", "hidden", "deferred", "unsupported", "ignored")
REALIZATION_FIELDS = (
    "draws",
    "sortedDraws",
    "billboardDraws",
    "pipelines",
    "materials",
    "textureViews",
    "samplers",
    "textureLoads",
    "textureCacheHits",
    "unsupportedTextureBindings",
    "runtimeContextEffects",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the OpenMW V4 CP3B4 real-NIF conformance corpus")
    parser.add_argument("--tool", required=True, type=pathlib.Path, help="openmw-vulkan-nif-conformance executable")
    parser.add_argument("--manifest", required=True, type=pathlib.Path, help="CP3B4 corpus manifest JSON")
    parser.add_argument("--data", action="append", default=[], type=pathlib.Path, help="OpenMW VFS data root; repeatable")
    parser.add_argument("--archive", action="append", default=[], type=pathlib.Path, help="BSA/BA2 archive; repeatable")
    parser.add_argument(
        "--encoding",
        choices=SUPPORTED_ENCODINGS,
        default="win1252",
        help="archive filename encoding; mirror the target OpenMW configuration (default win1252)",
    )
    parser.add_argument("--output", required=True, type=pathlib.Path, help="aggregate corpus report JSON")
    parser.add_argument("--lod-distance", type=float, default=0.0)
    parser.add_argument("--camera-distance", type=float, default=500.0)
    parser.add_argument("--render-id", action="append", default=[], help="asset id to visibly render instead of realize-only")
    parser.add_argument("--render-frames", type=int, default=120, help="frame count for each --render-id asset")
    parser.add_argument(
        "--determinism-runs",
        type=int,
        default=1,
        help="invoke each asset this many times and require byte-semantic identical tool reports",
    )
    parser.add_argument("--timeout", type=float, default=180.0, help="per-invocation timeout in seconds")
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_vfs_nif_path(nif: str, prefix: str) -> None:
    normalized = nif.replace("\\", "/")
    require(not normalized.startswith("/"), f"{prefix}.nif must be a VFS-relative path")
    require(
        not (len(normalized) >= 2 and normalized[0].isalpha() and normalized[1] == ":"),
        f"{prefix}.nif must not contain a drive-qualified path",
    )
    parts = normalized.split("/")
    require(
        all(part not in ("", ".", "..") for part in parts),
        f"{prefix}.nif must be a normalized VFS path without empty, '.' or '..' components",
    )
    require(normalized.lower().endswith(".nif"), f"{prefix}.nif must name a .nif asset")


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    require(isinstance(manifest, dict), "manifest root must be an object")
    require(manifest.get("schema") == CORPUS_SCHEMA, f"manifest schema must be {CORPUS_SCHEMA!r}")
    assets = manifest.get("assets")
    require(isinstance(assets, list) and assets, "manifest assets must be a non-empty array")

    required_tags = manifest.get("requiredTags", [])
    require(
        isinstance(required_tags, list) and all(isinstance(tag, str) and tag.strip() for tag in required_tags),
        "manifest requiredTags must be an array of non-empty strings",
    )
    require(len(set(required_tags)) == len(required_tags), "manifest requiredTags must not contain duplicates")

    seen_ids: set[str] = set()
    covered_tags: set[str] = set()
    for index, asset in enumerate(assets):
        prefix = f"assets[{index}]"
        require(isinstance(asset, dict), f"{prefix} must be an object")
        asset_id = asset.get("id")
        nif = asset.get("nif")
        require(isinstance(asset_id, str) and asset_id.strip(), f"{prefix}.id must be a non-empty string")
        require(asset_id not in seen_ids, f"duplicate asset id {asset_id!r}")
        seen_ids.add(asset_id)
        require(isinstance(nif, str) and nif.strip(), f"{prefix}.nif must be a non-empty VFS path")
        validate_vfs_nif_path(nif, prefix)
        expect = asset.get("expect", {})
        require(isinstance(expect, dict), f"{prefix}.expect must be an object")
        unknown = set(expect) - ({"complete", "minMeaningful"} | set(INTEGER_EXPECTATIONS))
        require(not unknown, f"{prefix}.expect has unknown keys: {sorted(unknown)}")
        if "complete" in expect:
            require(isinstance(expect["complete"], bool), f"{prefix}.expect.complete must be boolean")
        for key in set(expect) & (set(INTEGER_EXPECTATIONS) | {"minMeaningful"}):
            value = expect[key]
            require(
                isinstance(value, int) and not isinstance(value, bool) and value >= 0,
                f"{prefix}.expect.{key} must be a non-negative integer",
            )
        tags = asset.get("tags", [])
        require(
            isinstance(tags, list) and all(isinstance(tag, str) and tag.strip() for tag in tags),
            f"{prefix}.tags must be an array of non-empty strings",
        )
        require(len(set(tags)) == len(tags), f"{prefix}.tags must not contain duplicates")
        covered_tags.update(tags)
        if "class" in asset:
            require(
                isinstance(asset["class"], str) and asset["class"].strip(),
                f"{prefix}.class must be a non-empty string",
            )

    missing_tags = sorted(set(required_tags) - covered_tags)
    require(not missing_tags, f"manifest requiredTags are not covered by any asset: {missing_tags}")
    return manifest


def nested_integer(report: dict[str, Any], section: str, field: str) -> int:
    container = report.get(section)
    if not isinstance(container, dict):
        raise ValueError(f"tool report {section} is not an object")
    value = container.get(field)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"tool report {section}.{field} is not a non-negative integer")
    return value


def meaningful_outcomes(report: dict[str, Any]) -> int:
    return sum(nested_integer(report, "translation", field) for field in ("rendered", "collisionOnly", "hidden"))


def evaluate_policy(asset: dict[str, Any], report: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if report.get("schema") != ASSET_REPORT_SCHEMA:
        return [f"unexpected tool report schema {report.get('schema')!r}"]
    if report.get("nif") != asset["nif"]:
        errors.append(f"tool report nif {report.get('nif')!r} does not match manifest {asset['nif']!r}")

    expected = dict(DEFAULT_EXPECT)
    expected.update(asset.get("expect", {}))
    if report.get("complete") is not expected["complete"]:
        errors.append(f"complete={report.get('complete')!r}, expected {expected['complete']!r}")

    try:
        meaningful = meaningful_outcomes(report)
        if meaningful < expected["minMeaningful"]:
            errors.append(f"meaningful translation outcomes={meaningful} is below required minimum {expected['minMeaningful']}")
    except ValueError as exc:
        errors.append(str(exc))

    for key, (section, field, direction) in INTEGER_EXPECTATIONS.items():
        if key not in expected:
            continue
        try:
            actual = nested_integer(report, section, field)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        limit = expected[key]
        if direction == "min" and actual < limit:
            errors.append(f"{section}.{field}={actual} is below required minimum {limit}")
        elif direction == "max" and actual > limit:
            errors.append(f"{section}.{field}={actual} exceeds allowed maximum {limit}")

    return errors


def command_for_asset(args: argparse.Namespace, asset: dict[str, Any], report_path: pathlib.Path) -> list[str]:
    command = [str(args.tool)]
    for root in args.data:
        command.extend(["--data", str(root)])
    for archive in args.archive:
        command.extend(["--archive", str(archive)])
    command.extend(["--encoding", args.encoding])
    command.extend(["--nif", asset["nif"]])
    command.extend(["--lod-distance", str(args.lod_distance)])
    command.extend(["--camera-distance", str(args.camera_distance)])
    command.extend(["--report-json", str(report_path)])
    if asset["id"] in args.render_id:
        command.extend(["--frames", str(args.render_frames)])
    else:
        command.append("--realize-only")
    return command


def tail(text: str, limit: int = 16384) -> str:
    return text if len(text) <= limit else text[-limit:]


def invoke_once(args: argparse.Namespace, asset: dict[str, Any], report_path: pathlib.Path) -> dict[str, Any]:
    command = command_for_asset(args, asset, report_path)
    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return {
            "returnCode": None,
            "timedOut": True,
            "stdoutTail": tail(exc.stdout or ""),
            "stderrTail": tail(exc.stderr or ""),
            "toolReport": None,
            "invocationErrors": [f"tool timed out after {args.timeout} seconds"],
        }

    invocation_errors: list[str] = []
    tool_report: dict[str, Any] | None = None
    if report_path.is_file():
        try:
            with report_path.open("r", encoding="utf-8") as stream:
                decoded = json.load(stream)
            if isinstance(decoded, dict):
                tool_report = decoded
            else:
                invocation_errors.append("tool report root is not an object")
        except (OSError, json.JSONDecodeError) as exc:
            invocation_errors.append(f"unable to read tool report: {exc}")
    else:
        invocation_errors.append("tool did not create --report-json output")

    if completed.returncode != 0:
        invocation_errors.append(f"tool exited with {completed.returncode}")

    return {
        "returnCode": completed.returncode,
        "timedOut": False,
        "stdoutTail": tail(completed.stdout),
        "stderrTail": tail(completed.stderr),
        "toolReport": tool_report,
        "invocationErrors": invocation_errors,
    }


def canonical_report(report: dict[str, Any]) -> str:
    return json.dumps(report, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def run_asset(args: argparse.Namespace, asset: dict[str, Any], work_dir: pathlib.Path) -> dict[str, Any]:
    runs: list[dict[str, Any]] = []
    baseline: str | None = None
    determinism_errors: list[str] = []

    for run_index in range(args.determinism_runs):
        report_path = work_dir / f"{asset['id']}-{run_index}.json"
        invocation = invoke_once(args, asset, report_path)
        runs.append(invocation)
        report = invocation["toolReport"]
        if not invocation["invocationErrors"] and report is not None:
            current = canonical_report(report)
            if baseline is None:
                baseline = current
            elif current != baseline:
                determinism_errors.append(f"run {run_index + 1} tool report differs from run 1")

    first_report = runs[0]["toolReport"] if runs else None
    policy_errors: list[str] = []
    if first_report is not None:
        policy_errors.extend(evaluate_policy(asset, first_report))
    else:
        policy_errors.append("no valid tool report available for policy evaluation")

    invocation_errors = [error for run in runs for error in run["invocationErrors"]]
    errors = invocation_errors + determinism_errors + policy_errors
    return {
        "id": asset["id"],
        "nif": asset["nif"],
        "class": asset.get("class", "unspecified"),
        "tags": asset.get("tags", []),
        "expect": {**DEFAULT_EXPECT, **asset.get("expect", {})},
        "passed": not errors,
        "errors": errors,
        "toolReport": first_report,
        "runs": [
            {
                "returnCode": run["returnCode"],
                "timedOut": run["timedOut"],
                "stdoutTail": run["stdoutTail"],
                "stderrTail": run["stderrTail"],
            }
            for run in runs
        ],
    }


def aggregate_counts(assets: list[dict[str, Any]], section: str, fields: tuple[str, ...]) -> dict[str, int]:
    totals = {field: 0 for field in fields}
    for asset in assets:
        report = asset.get("toolReport")
        if not isinstance(report, dict) or report.get("schema") != ASSET_REPORT_SCHEMA:
            continue
        for field in fields:
            try:
                totals[field] += nested_integer(report, section, field)
            except ValueError:
                pass
    return totals


def main() -> int:
    args = parse_args()
    try:
        require(args.data, "at least one --data root is required")
        require(args.determinism_runs >= 1, "--determinism-runs must be at least 1")
        require(args.render_frames >= 0, "--render-frames must be non-negative")
        require(args.timeout > 0, "--timeout must be greater than zero")
        require(args.tool.is_file(), f"tool does not exist: {args.tool}")
        require(args.manifest.is_file(), f"manifest does not exist: {args.manifest}")
        manifest = load_manifest(args.manifest)
        asset_ids = {asset["id"] for asset in manifest["assets"]}
        unknown_render_ids = set(args.render_id) - asset_ids
        require(not unknown_render_ids, f"--render-id not present in manifest: {sorted(unknown_render_ids)}")

        with tempfile.TemporaryDirectory(prefix="openmw-cp3b4-") as temp_dir:
            work_dir = pathlib.Path(temp_dir)
            assets = [run_asset(args, asset, work_dir) for asset in manifest["assets"]]

        passed = all(asset["passed"] for asset in assets)
        covered_tags = sorted({tag for asset in manifest["assets"] for tag in asset.get("tags", [])})
        aggregate = {
            "schema": CORPUS_REPORT_SCHEMA,
            "manifestSchema": manifest["schema"],
            "suite": manifest.get("suite", "unnamed"),
            "toolSha256": sha256_file(args.tool),
            "manifestSha256": sha256_file(args.manifest),
            "encoding": args.encoding,
            "determinismRuns": args.determinism_runs,
            "requiredTags": manifest.get("requiredTags", []),
            "coveredTags": covered_tags,
            "passed": passed,
            "summary": {
                "assets": len(assets),
                "passed": sum(1 for asset in assets if asset["passed"]),
                "failed": sum(1 for asset in assets if not asset["passed"]),
                "translationTotals": aggregate_counts(assets, "translation", TRANSLATION_FIELDS),
                "realizationTotals": aggregate_counts(assets, "realization", REALIZATION_FIELDS),
            },
            "assets": assets,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(aggregate, stream, indent=2, sort_keys=True, ensure_ascii=False)
            stream.write("\n")

        for asset in assets:
            state = "PASS" if asset["passed"] else "FAIL"
            print(f"{state} {asset['id']}: {asset['nif']}")
            for error in asset["errors"]:
                print(f"  - {error}")
        print(f"CP3B4 corpus: {aggregate['summary']['passed']}/{aggregate['summary']['assets']} assets passed")
        print(f"CP3B4 report: {args.output}")
        return 0 if passed else 1
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"CP3B4 corpus configuration failure: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
