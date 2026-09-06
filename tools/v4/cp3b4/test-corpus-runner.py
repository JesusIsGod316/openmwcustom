#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import stat
import subprocess
import sys
import tempfile


def write_fake_tool(path: pathlib.Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import json
import pathlib
import sys

args = sys.argv[1:]
def value(name):
    return args[args.index(name) + 1]

nif = value('--nif')
report_path = pathlib.Path(value('--report-json'))
unsupported = 1 if 'unsupported' in nif else 0
report = {
    'schema': 'openmw-v4-cp3b4-asset-report-v1',
    'nif': nif,
    'complete': True,
    'stage': 'complete',
    'publishStatus': 'applied',
    'translation': {
        'rendered': 1,
        'collisionOnly': 0,
        'hidden': 0,
        'deferred': 0,
        'unsupported': unsupported,
        'ignored': 0,
    },
    'realization': {
        'draws': 1,
        'sortedDraws': 0,
        'billboardDraws': 0,
        'pipelines': 1,
        'materials': 1,
        'textureViews': 0,
        'samplers': 0,
        'textureLoads': 0,
        'textureCacheHits': 0,
        'unsupportedTextureBindings': 0,
        'runtimeContextEffects': 0,
    },
    'translationDiagnostics': [],
    'realizationDiagnostics': [],
}
report_path.write_text(json.dumps(report), encoding='utf-8')
""",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def prepare(root: pathlib.Path, manifest: dict) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path]:
    root.mkdir(parents=True, exist_ok=True)
    tool = root / "fake-conformance"
    data = root / "data"
    manifest_path = root / "manifest.json"
    output = root / "report.json"
    data.mkdir(exist_ok=True)
    write_fake_tool(tool)
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return tool, data, manifest_path, output


def run_runner(root: pathlib.Path, manifest: dict, expected_exit: int, runs: int = 2) -> dict:
    runner = pathlib.Path(__file__).with_name("corpus-runner.py")
    tool, data, manifest_path, output = prepare(root, manifest)

    completed = subprocess.run(
        [
            sys.executable,
            str(runner),
            "--tool",
            str(tool),
            "--manifest",
            str(manifest_path),
            "--data",
            str(data),
            "--output",
            str(output),
            "--determinism-runs",
            str(runs),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != expected_exit:
        raise AssertionError(
            f"runner exit {completed.returncode}, expected {expected_exit}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not output.is_file():
        raise AssertionError("runner did not write aggregate report")
    return json.loads(output.read_text(encoding="utf-8"))


def require_config_failure(root: pathlib.Path, manifest: dict, expected_text: str) -> None:
    runner = pathlib.Path(__file__).with_name("corpus-runner.py")
    tool, data, manifest_path, output = prepare(root, manifest)
    completed = subprocess.run(
        [
            sys.executable,
            str(runner),
            "--tool",
            str(tool),
            "--manifest",
            str(manifest_path),
            "--data",
            str(data),
            "--output",
            str(output),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 2 or expected_text not in completed.stderr:
        raise AssertionError(
            f"expected configuration failure containing {expected_text!r}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if output.exists():
        raise AssertionError("configuration failure unexpectedly created aggregate report")


def main() -> int:
    good_manifest = {
        "schema": "openmw-v4-cp3b4-corpus-v1",
        "suite": "synthetic-runner-contract",
        "requiredTags": ["opaque"],
        "assets": [
            {
                "id": "opaque-static",
                "nif": "meshes/test/opaque.nif",
                "class": "synthetic",
                "tags": ["opaque"],
                "expect": {"minRendered": 1, "minDraws": 1},
            }
        ],
    }
    bad_manifest = {
        "schema": "openmw-v4-cp3b4-corpus-v1",
        "suite": "synthetic-policy-failure",
        "assets": [
            {
                "id": "unsupported-static",
                "nif": "meshes/test/unsupported.nif",
                "class": "synthetic",
                "expect": {"minRendered": 1},
            }
        ],
    }

    with tempfile.TemporaryDirectory(prefix="cp3b4-runner-test-") as temp:
        root = pathlib.Path(temp)
        good = run_runner(root / "good", good_manifest, 0)
        summary = good["summary"]
        if not good["passed"] or summary["assets"] != 1 or summary["passed"] != 1 or summary["failed"] != 0:
            raise AssertionError(f"unexpected passing aggregate: {good}")
        if summary["translationTotals"]["rendered"] != 1 or summary["realizationTotals"]["draws"] != 1:
            raise AssertionError(f"aggregate disposition totals are wrong: {summary}")
        if good["requiredTags"] != ["opaque"] or "opaque" not in good["coveredTags"]:
            raise AssertionError(f"aggregate coverage identity is wrong: {good}")
        for key in ("toolSha256", "manifestSha256"):
            value = good.get(key, "")
            if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
                raise AssertionError(f"{key} is not a SHA-256 identity: {value!r}")

        bad = run_runner(root / "bad", bad_manifest, 1, runs=1)
        if bad["passed"] or bad["summary"]["failed"] != 1:
            raise AssertionError(f"unexpected failing aggregate: {bad}")
        errors = bad["assets"][0]["errors"]
        if not any("unsupported" in error.lower() for error in errors):
            raise AssertionError(f"policy failure did not identify unsupported content: {errors}")

        missing_coverage = {
            "schema": "openmw-v4-cp3b4-corpus-v1",
            "requiredTags": ["opaque", "alpha-test"],
            "assets": [{"id": "opaque", "nif": "meshes/opaque.nif", "tags": ["opaque"]}],
        }
        require_config_failure(root / "missing-coverage", missing_coverage, "requiredTags")

        unsafe_path = {
            "schema": "openmw-v4-cp3b4-corpus-v1",
            "assets": [{"id": "unsafe", "nif": "C:\\Games\\Data Files\\unsafe.nif"}],
        }
        require_config_failure(root / "unsafe-path", unsafe_path, "drive-qualified")

    print("CP3B4 corpus runner contract tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
