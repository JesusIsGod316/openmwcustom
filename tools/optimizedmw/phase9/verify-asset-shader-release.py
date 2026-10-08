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
import xml.etree.ElementTree as ET


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


def population(text, marker, identity_key, identities, expected):
    """Each required production family must execute once in this exact receipt."""
    rows = re.findall(r"^" + re.escape(marker) + r" (.*)$", text, re.M)
    require(len(rows) == len(identities), "Missing/extra production family in " + marker)
    result = {}
    for row in rows:
        fields = record(marker + " " + row + "\n", marker, expected)
        identity = fields.get(identity_key)
        require(identity in identities and identity not in result, "Unknown/duplicate family in " + marker)
        result[identity] = fields
    require(set(result) == set(identities), "Incomplete family population in " + marker)
    return result


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
                     ("p9-asset-shader-contract", "p9-asset-material-runtime", "p9-asset-admission",
                      "p9-generated-static-lod", "p9-temporal-asset-producer")}
    require(set(extra.get("input_hashes", {})) == fixture_paths, "Asset fixture executable population is incomplete")
    current_inputs(extra["input_hashes"])
    configurations = {str((folder / name).resolve()) for name in ("current-openmw.cfg", "current-settings.cfg")}
    require(set(extra.get("configuration_hashes", {})) == configurations, "Current configuration fixture inputs incomplete")
    current_inputs(extra["configuration_hashes"])
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
                "GEOMETRY-L1-CHAIN.txt", "GEOMETRY-H1-CHAIN.txt", "PACKAGE-QUALIFIER-TESTS-FINAL.txt", "ASSET-SHADER-GATE-TESTS-FINAL.txt",
                "ASSET-ADMISSION.txt", "EFFECTIVE-MATERIAL-OWNERSHIP.txt", "TEMPORAL-ASSET-PRODUCER.txt",
                "ASSET-SHADER-TEMPORAL-ONLY.txt", "GEOMETRY-L1-CURRENT.txt",
                "EXACT-VARIANT-READER-TESTS.txt", "EXACT-VARIANT-READER-TESTS.xml"}
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
    variant_xml = texts["EXACT-VARIANT-READER-TESTS.xml"]
    require("<!DOCTYPE" not in variant_xml.upper() and "<!ENTITY" not in variant_xml.upper(), "Unexpected variant XML entity")
    variant_cases = list(ET.fromstring(variant_xml).iter("testcase"))
    require(len(variant_cases) == 2 and {case.get("name") for case in variant_cases} ==
            {"FactorySeparatesActualTemplateBindingsAndSamplers", "ProducerRelinkRestoresActualSamplerUniformsOnTheSamePcp"}
            and all(case.get("classname") == "ProgramVariant" and case.get("status") == "run"
                    and case.get("result") == "completed" and not list(case.iter("failure"))
                    and not list(case.iter("error")) and not list(case.iter("skipped")) for case in variant_cases),
            "Exact variant reader transaction tests failed or skipped")
    baseline = record(texts["ASSET-SHADER-BASELINE.txt"], "ASSET_SHADER_BASELINE_PASS",
                      {"stock_and_pbr": 1, "pinned_overlay": 1, "installed_authority": 0})
    contract = record(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_PASS",
                      {"stock": 1, "pbr": 1, "source_negative": 1, "link_negative": 1,
                       "bindings_negative": 1, "defines_negative": 1, "epoch_negative": 1,
                       "path_negative": 1, "indirect_unchanged": 1, "metadata_bounded": 1,
                       "cached_refresh": 1, "l1_source": 1, "old_snapshot_rejected": 1,
                       "no_op": 1, "mutation_refresh_rejected": 1, "refresh_capacity": 1})
    refresh = population(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_REFRESH", "family", {"stock", "pbr"},
                         {"cached": 1, "old_snapshot_rejected": 1, "current_recipe": 1, "l1_source": 1,
                          "no_op": 1, "slot_replaced": 1, "bounded": 1})
    population(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_REFRESH_NEGATIVE", "mutation", {str(n) for n in range(8)}, {"rejected": 1})
    concurrency = record(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_CONCURRENCY",
                         {"readers": 2, "writes": 16, "coherent_snapshots": 1, "no_deadlock": 1, "stale_refused": 1})
    capacity = record(texts["ASSET-SHADER-CONTRACT.txt"], "ASSET_CONTRACT_REFRESH_CAPACITY", {"cap": 4194304, "fallback": 1})
    require(all(re.fullmatch(r"[0-9]+", capacity.get(key, "")) for key in ("retained", "metadata"))
            and int(capacity["retained"]) > 0 and int(capacity["metadata"]) <= 4194304, "Refresh metadata quota was not exercised safely")
    temporal_only = record(texts["ASSET-SHADER-TEMPORAL-ONLY.txt"], "ASSET_CONTRACT_PASS",
                           {key: 1 for key in ("stock", "pbr", "source_negative", "link_negative", "bindings_negative",
                            "defines_negative", "epoch_negative", "path_negative", "indirect_unchanged", "metadata_bounded",
                            "cached_refresh", "l1_source", "old_snapshot_rejected", "no_op", "mutation_refresh_rejected",
                            "refresh_capacity", "temporal_only")})
    record(texts["ASSET-SHADER-TEMPORAL-ONLY.txt"], "ASSET_CONTRACT_CONCURRENCY",
           {"readers": 2, "writes": 16, "coherent_snapshots": 1, "no_deadlock": 1, "stale_refused": 1})
    temporal = record(texts["TEMPORAL-ASSET-PRODUCER.txt"], "TEMPORAL_ASSET_PASS",
                      {key: 1 for key in ("stock", "pbr", "index_lineage", "real_motion_pixels", "opaque_only",
                       "current_material", "bounded", "no_dense_claim", "no_dlss_claim", "indirect_unchanged", "temporal_only")})
    population(texts["TEMPORAL-ASSET-PRODUCER.txt"], "TEMPORAL_ASSET_FAMILY", "family", {"stock", "pbr"},
               {key: 1 for key in ("source", "tier_switch", "tier_return", "motion_pixels", "previous_pose", "effective_program",
                "blend_override", "alpha_override", "shader_cutout", "source_mutation", "binding_mutation", "material_mutation",
                "callback", "epoch", "projection", "projection_mutation", "previous_projection",
                "missing_projection", "stale_lineage", "indirect_unchanged")})
    runtime = record(texts["ASSET-MATERIAL-RUNTIME.txt"], "ASSET_MATERIAL_PASS",
                     {"l1": 1, "h1": 1, "source_mutation": 1, "derived_mutation": 1,
                      "cull_fallback": 1, "draw_fallback": 1, "control_preserved": 1, "gl_error": 0})
    reversed_runtime = record(texts["ASSET-MATERIAL-RUNTIME-REVERSED.txt"], "ASSET_MATERIAL_PASS",
                              {"l1": 1, "h1": 1, "source_mutation": 1, "derived_mutation": 1,
                               "cull_fallback": 1, "draw_fallback": 1, "control_preserved": 1, "gl_error": 0})
    record(texts["ASSET-MATERIAL-RUNTIME-REVERSED.txt"], "ASSET_MATERIAL_PROJECTION",
           {"reversed": 1, "arbitrary_projection_refused": 1, "clipcontrol_refused": 1, "gl_error": 0})
    ownership = record(texts["EFFECTIVE-MATERIAL-OWNERSHIP.txt"], "L1_EFFECTIVE_MATERIAL_OWNERSHIP",
                       {key: 1 for key in ("leaf_own", "inherited", "sibling_restore", "override_protected",
                        "shared_ordinary", "family_reunion", "null_refused", "unknown_refused", "capacity_refused")})
    record(texts["EFFECTIVE-MATERIAL-OWNERSHIP.txt"], "L1_TEMPORAL_DRAW_RECIPE", {key: 1 for key in
           ("exact_owner", "borrowed_owner_refused", "source_uv_refused", "source_material_refused", "derived_indices_refused", "restored_exact")})
    admission = record(texts["ASSET-ADMISSION.txt"], "ASSET_ADMISSION_PASS",
                       {key: 1 for key in ("fixed_cap", "exact_private_progress", "aggregate_denial", "coherent_receipt",
                        "headroom", "unknown_pressure", "owner_release", "growth_conserved", "overflow", "demand_unblocked",
                        "retry_cooldown", "finite_tail", "generation_reset", "cancellation", "terminal_fallback")})
    chain = {}
    for mask, name in ((1, "GEOMETRY-L1-CHAIN.txt"), (2, "GEOMETRY-H1-CHAIN.txt"), (1, "GEOMETRY-L1-CURRENT.txt")):
        chain[name] = record(texts[name], "CHAIN_PACKAGE",
                             {"mask": mask, "packaged_shaders": 1, "captured_settings": 1,
                              "define_substitutions": 0, "stock_certificate_injection": 0})
        record(texts[name], "CHAIN_SHADER_MUTATION", {"published": 1, "fallback_original": 1, "derived_draws": 0, "gl_error": 0})
        record(texts[name], "CHAIN_SHADOW", {"real_shadow_cull": 1, "derived_draws": 0, "gl_error": 0})
        record(texts[name], "CHAIN_POSTPROCESS", {"shader_global_policy_mirrored": 1, "output_canvas_qualified": 0})
        record(texts[name], "CHAIN_SHADER_COHORT", {key: 1 for key in
            ("unchanged_no_generation", "unchanged_no_recook", "old_guard_original", "stale_caller_cancelled",
             "fresh_owner_preserved", "source_recertified", "new_cohort_recooked", "fresh_actual_draw",
             "restored_actual_draw", "old_snapshot_still_epoch") } | {"gl_error": 0})
        record(texts[name], "CHAIN_RETRY_OBSERVATIONS", {"cooldown_signalled": 1, "pending_ico_signalled": 1,
            "ready_stops_retry": 1, "cache_observation_recooks": 0})
        record(texts[name], "CHAIN_RETRY_COALESCED", {"inflight_signalled": 1, "exact_weak_owner": 1,
            "observer_build_attempts": 0, "peer_published": 1})
        record(texts[name], "CHAIN_RETRY_EXHAUSTED", {"actual_attempts": 3, "settled_stops_retry": 1,
            "ordinary_preserved": 1, "builder_calls": 0, "generation_reset": 0})
        if mask == 1:
            mutations = population(texts[name], "CHAIN_TEMPORAL_RECIPE_MUTATION", "kind",
                                   {"source_uv", "source_material", "derived_indices"},
                                   {"captured_current": 1, "changed_after_cull": 1, "temporal_recipe_refused": 1,
                                    "derived_draws": 0, "gl_error": 0})
            require(all(all(re.fullmatch(r"[0-9]+", row.get(key, "")) and int(row[key]) > minimum
                            for key, minimum in (("original_draws", 0), ("callback_fallbacks", 0), ("rgb", 16)))
                        for row in mutations.values()), "Temporal recipe mutation did not prove actual original draw coverage")
        if mask == 2:
            record(texts[name], "CHAIN_SHADER_FEATURES", {"normal_map_coverage": 1, "required_h1_normal_map": 1})
        else:
            record(texts[name], "CHAIN_MIXED_INPUT", {"candidate_instances": 2, "spatial_buckets": 2,
                   "ordinary_instances": 1, "actual_stat_sources": 1, "material_substitution": 0})
            mixed = record(texts[name], "CHAIN_MIXED_CONTROL", {"spatial_buckets": 2, "ordinary_preserved": 1, "extra_control_submissions": 0})
            require(all(re.fullmatch(r"[0-9]+", mixed.get(key, "")) for key in
                        ("baseline_submissions", "retained_submissions", "baseline_indices", "retained_indices", "cohesive_partitions"))
                    and 0 < int(mixed["retained_submissions"]) <= int(mixed["baseline_submissions"])
                    and 0 < int(mixed["baseline_indices"]) == int(mixed["retained_indices"])
                    and int(mixed["cohesive_partitions"]) > 0, "Mixed production control batching or family cohesion not proved")
            record(texts[name], "CHAIN_MIXED_CONSUMED", {"mixed_ordinary_candidate": 1, "real_multi_bucket_cook": 1,
                   "actual_main_draw": 1, "fewer_indices": 1, "control_batching_preserved": 1, "gl_error": 0})
            shared = record(texts[name], "CHAIN_MIXED_SHARED_REFUSAL", {"ordinary_conflict_refused": 1, "build_attempted": 0,
                            "exact_indices": 1, "control_preserved": 1, "terminal_stops_retry": 1})
            require(shared.get("baseline_submissions") == shared.get("fallback_submissions"), "Shared ordinary material did not preserve exact fallback submissions")
    return {"baseline": baseline, "contract": contract, "cached_refresh": refresh, "shader_concurrency": concurrency,
            "ownership": ownership, "admission": admission,
            "runtime": runtime, "reversed_runtime": reversed_runtime, "package_chains": chain,
            "temporal_only_provenance": temporal_only, "opaque_motion_producer": temporal,
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
