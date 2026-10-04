"""Additional fail-closed package lane for the asset shader compatibility repair.

The retained exact-build release verifier must pass separately. This lane binds
actual package/captured settings and the shader/runtime fixtures to that same
final execution. Passing it qualifies packaging, never gameplay performance.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


class QualificationError(ValueError):
    pass


def require(value, reason):
    if not value:
        raise QualificationError(reason)


def sha(path):
    path = Path(path)
    before = path.stat()
    require(path.is_file() and before.st_size > 0 and not path.is_symlink(), "Missing/linked input: " + str(path))
    with path.open("rb") as stream:
        value = hashlib.file_digest(stream, "sha256").hexdigest()
    after = path.stat()
    require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), "Input changed while hashing: " + str(path))
    return value


def load(path):
    require(Path(path).stat().st_size <= 64 * 1024 * 1024, "Oversize qualification receipt")
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def record(text, marker, expected):
    require(not re.search(r"^(?:SKIP:|.*_FAILURE|\[\s*SKIPPED\s*\])", text, re.M), "Skipped/failed asset qualification")
    rows = re.findall(r"^" + re.escape(marker) + r" (.*)$", text, re.M)
    require(len(rows) == 1, "Missing/ambiguous " + marker)
    pairs = re.findall(r"\b([A-Za-z0-9_]+)=([^\s]+)", rows[0])
    fields = dict(pairs)
    require(len(pairs) == len(fields), "Duplicate fields in " + marker)
    for key, value in expected.items():
        require(fields.get(key) == str(value), marker + " missing proof: " + key)
    return fields


def current_inputs(values):
    require(bool(values), "Execution input population absent")
    for path, digest in values.items():
        require(re.fullmatch(r"[a-fA-F0-9]{64}", digest) is not None and sha(path) == digest.lower(), "Changed execution input: " + path)


def evaluate(folder, commit, source, game, workspace, installed=None):
    base = load(folder / "RELEASE-GATES.json")
    require(base.get("source_commit") == commit and base.get("deployment_prerequisite_pass") is True,
            "Retained exact-build release prerequisites did not pass")
    require(len(base.get("gates", {})) == 20 and all(g.get("status") == "PASS" for g in base["gates"].values()),
            "Retained release gate population changed or failed")
    main_run = load(folder / "FINAL-RUN-EXECUTION.json")
    extra = load(folder / "ASSET-SHADER-RUN-EXECUTION.json")
    require(main_run.get("source_commit") == commit == extra.get("source_commit"), "Asset/source execution mismatch")
    require(extra.get("all_commands_exit_zero") is True, "Asset fixture commands did not all finish successfully")
    start = datetime.fromisoformat(main_run["started_utc"]).timestamp()
    finish = datetime.fromisoformat(main_run["finished_utc"]).timestamp()
    extra_start = datetime.fromisoformat(extra["started_utc"]).timestamp()
    extra_finish = datetime.fromisoformat(extra["finished_utc"]).timestamp()
    require(start <= extra_start <= extra_finish <= finish, "Asset fixtures outside the exact final execution window")
    fixture_paths = {str((game / (name + ".exe")).resolve()) for name in
                     ("p9-asset-shader-contract", "p9-asset-material-runtime")}
    require(set(extra.get("input_hashes", {})) == fixture_paths, "Asset fixture executable population is incomplete")
    current_inputs(extra["input_hashes"])
    current_inputs(main_run["input_hashes"])
    identity = load(folder / "RELEASE-INPUT-IDENTITY.json")
    require(identity.get("commit") == commit, "Retained final binding is stale")
    require(all(sha(folder / name) == digest for name, digest in identity["receipt_sha256"].items()),
            "Retained exact-run receipts changed")
    proof_path = workspace / "PACKAGE-QUALIFICATION-INPUT.json"
    restored_path = workspace / "PACKAGE-QUALIFICATION-UNCHANGED.json"
    require(extra.get("package_input_sha256") == sha(proof_path), "Asset package inputs unbound")
    require(extra.get("package_unchanged_sha256") == sha(restored_path), "Restored package receipt unbound")
    proof = load(proof_path)
    require(proof.get("schema") == "optimizedmw.asset-paging-package-input.v1" and proof.get("shader_copy_exact") is True,
            "Exact package shader copy was not established")
    require(Path(proof["package_root"]).resolve() == game.resolve(), "Fixture did not consume the final built package")
    require(Path(proof["shader_workspace"]).resolve() == (workspace / "shaders").resolve(), "Shader mirror escaped qualification workspace")
    helper = source / "tools/optimizedmw/phase9/qualify-asset-paging-package.py"
    result = subprocess.run([sys.executable, str(helper), "--verify", str(proof_path), "--read-only"],
                            capture_output=True, text=True, timeout=180, check=False)
    require(result.returncode == 0, "Package/capture inputs or restored shader mirror changed: " + result.stderr[-2000:])
    require(sha(restored_path) == extra["package_unchanged_sha256"], "Restored receipt changed during current check")
    if installed is not None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("qualified_package", helper)
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        require(package.inventory(installed / "resources") == proof["resource_files"], "Installed resource closure differs from the qualified package")
        require(sha(installed / "defaults.bin") == proof["immutable_inputs"]["defaults"]["sha256"], "Installed defaults differ from the qualified package")
        require(sha(installed / "openmw.exe") == base["gates"]["built_exe_pdb"]["details"]["exe"]["sha256"], "Installed executable differs from the qualified build")
    receipts = extra.get("receipt_sha256", {})
    required = {"ASSET-SHADER-CONTRACT.txt", "ASSET-MATERIAL-RUNTIME.txt", "ASSET-MATERIAL-RUNTIME-REVERSED.txt", "ASSET-SHADER-BASELINE.txt",
                "GEOMETRY-L1-CHAIN.txt", "GEOMETRY-H1-CHAIN.txt", "PACKAGE-QUALIFIER-TESTS-FINAL.txt", "ASSET-SHADER-GATE-TESTS-FINAL.txt"}
    require(set(receipts) == required, "Asset result receipt population incomplete")
    texts = {}
    for name, digest in receipts.items():
        path = folder / name
        require(sha(path) == digest, "Asset fixture result changed: " + name)
        require(start - 1 <= path.stat().st_mtime <= finish + 1, "Asset fixture result is stale: " + name)
        texts[name] = path.read_text(encoding="utf-8-sig")
    for name in ("PACKAGE-QUALIFIER-TESTS-FINAL.txt", "ASSET-SHADER-GATE-TESTS-FINAL.txt"):
        require(re.findall(r"^Ran (\d+) tests in [\d.]+s\s*$", texts[name], re.M) == ["8"]
                and re.search(r"\nOK\s*$", texts[name]) and "skipped=" not in texts[name], "Package gate regression suite did not execute eight complete passes: " + name)
    baseline = record(texts["ASSET-SHADER-BASELINE.txt"], "ASSET_SHADER_BASELINE_PASS",
                      {"stock_and_pbr": 1, "pinned_overlay": 1, "installed_authority": 0})
    contract = record(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_PASS",
                      {"stock": 1, "pbr": 1, "source_negative": 1, "link_negative": 1,
                       "bindings_negative": 1, "defines_negative": 1, "epoch_negative": 1,
                       "path_negative": 1, "indirect_unchanged": 1, "metadata_bounded": 1})
    runtime = record(texts["ASSET-MATERIAL-RUNTIME.txt"], "ASSET_MATERIAL_PASS",
                     {"l1": 1, "h1": 1, "source_mutation": 1, "derived_mutation": 1,
                      "cull_fallback": 1, "draw_fallback": 1, "control_preserved": 1, "gl_error": 0})
    reversed_runtime = record(texts["ASSET-MATERIAL-RUNTIME-REVERSED.txt"], "ASSET_MATERIAL_PASS",
                              {"l1": 1, "h1": 1, "source_mutation": 1, "derived_mutation": 1,
                               "cull_fallback": 1, "draw_fallback": 1, "control_preserved": 1, "gl_error": 0})
    record(texts["ASSET-MATERIAL-RUNTIME-REVERSED.txt"], "ASSET_MATERIAL_PROJECTION",
           {"reversed": 1, "arbitrary_projection_refused": 1, "clipcontrol_refused": 1, "gl_error": 0})
    chain = {}
    for mask, name in ((1, "GEOMETRY-L1-CHAIN.txt"), (2, "GEOMETRY-H1-CHAIN.txt")):
        chain[name] = record(texts[name], "CHAIN_PACKAGE",
                             {"mask": mask, "packaged_shaders": 1, "captured_settings": 1,
                              "define_substitutions": 0, "stock_certificate_injection": 0})
        record(texts[name], "CHAIN_SHADER_MUTATION", {"published": 1, "fallback_original": 1, "derived_draws": 0, "gl_error": 0})
        record(texts[name], "CHAIN_SHADOW", {"real_shadow_cull": 1, "derived_draws": 0, "gl_error": 0})
        record(texts[name], "CHAIN_POSTPROCESS", {"shader_global_policy_mirrored": 1, "output_canvas_qualified": 0})
        if mask == 2:
            record(texts[name], "CHAIN_SHADER_FEATURES", {"normal_map_coverage": 1, "required_h1_normal_map": 1})
    return {"baseline": baseline, "contract": contract, "runtime": runtime, "reversed_runtime": reversed_runtime, "package_chains": chain,
            "package_input_sha256": extra["package_input_sha256"],
            "fixture_executables": extra["input_hashes"], "source_commit": commit,
            "installed_root": str(installed) if installed else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--package-workspace", type=Path, required=True)
    parser.add_argument("--installed-root", type=Path)
    args = parser.parse_args()
    require(re.fullmatch(r"[a-f0-9]{40}", args.commit) is not None, "Full source commit required")
    report = {"schema": "optimizedmw.asset-shader-release.v1", "source_commit": args.commit,
              "deployment_prerequisite_pass": False, "performance_acceptance": "PENDING_USER_GAMEPLAY"}
    try:
        report["details"] = evaluate(args.evidence_root, args.commit, args.source_root, args.package_root, args.package_workspace, args.installed_root)
        report["deployment_prerequisite_pass"] = True
        report["status"] = "PACKAGE_SHADER_PREREQUISITES_PASS"
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report.update(status="BLOCKED", reason=str(error))
    (args.evidence_root / "ASSET-SHADER-RELEASE-GATES.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report["status"] + (": " + report["reason"] if "reason" in report else ""))
    return 0 if report["deployment_prerequisite_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
