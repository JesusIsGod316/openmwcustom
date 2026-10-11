"""Seal finished real adapter and negative-control evidence; never launches a process."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path: Path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": result.hexdigest()}


def records(path: Path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def checked_run(directory: Path, negative: bool):
    inputs = read_json(directory / "INPUTS.json")
    receipt = read_json(directory / "Desktop" / "DESKTOP-RECEIPT.json")
    assert inputs["negative_control"] is negative and inputs["profile_mutex_held"]
    assert not inputs["game_package_changed"]
    assert receipt["state"] == "exited" and receipt["exit_code"] == 0 and not receipt["timed_out"]
    assert not receipt["desktop_switched"] and receipt["input_desktop_unchanged"]
    assert not receipt["input_desktop_owned_visible_windows"]
    for source in inputs["source_files"]:
        assert digest(Path(source["path"]))["sha256"] == source["sha256"], "Source changed since native proof"
    evidence = records(directory / "adapter-evidence.jsonl")
    # On the expected negative rejection, main's exception handler emits the
    # final marker to stdout after the evidence stream closes. Keep and check
    # both raw artifacts; a native exit code alone is never sufficient proof.
    stdout = records(directory / "Desktop" / "stdout.log")
    assert all(row.get("status") != "fail" for row in evidence + stdout)
    device = [r for r in evidence if r["check"] == "same_device_real_ngx"]
    assert len(device) == 1 and device[0]["same_device"] and device[0]["window_hidden"]
    assert device[0]["device_node_mask"] and device[0]["actual_ngx_backend"] == "D3D11"
    if negative:
        assert not any(r["check"] == "real_dlss_evaluated" for r in evidence + stdout)
        marker = [r for r in stdout if r["check"] == "negative_no_evaluation"]
        assert len(marker) == 1 and marker[0]["status"] == "pass"
        assert marker[0]["untouched_output_rejected"] and marker[0]["actual_dlss_evaluations"] == 0
    else:
        markers = [r for r in evidence if r["check"] == "adapter_complete"]
        assert len(markers) == 1 and markers[0]["status"] == "pass"
        assert markers[0]["actual_evaluations"] == 12 and markers[0]["retired_generations"] == 3
        assert not markers[0]["game_dlss_enabled"] and not markers[0]["dense_game_motion_proved"]
        evaluations = [r for r in evidence if r["check"] == "real_dlss_evaluated"]
        assert len(evaluations) == 12
        assert [(r["generation"], r["sequence"]) for r in evaluations] == [
            (generation, 4 * (generation - 1) + sequence)
            for generation in range(1, 4) for sequence in range(1, 5)]
        for row in evaluations:
            assert row["status"] == "pass" and row["exact_input_pixels"] and row["exact_cross_api_output"]
            assert row["exact_exposure_pixel"]
            assert row["reset"] is (row["sequence"] % 4 in (1, 3)) and row["maximum_patch_error"] < .15
        for check in ("feature_retirement_gpu_complete", "feature_retirement_sdk_released",
                      "feature_retirement_resources_released"):
            assert len([r for r in evidence if r["check"] == check and r["status"] == "pass"]) == 3
    return inputs, receipt, evidence, [digest(directory / relative) for relative in (
        "INPUTS.json", "adapter-evidence.jsonl", "Desktop/stdout.log", "Desktop/stderr.log",
        "Desktop/DESKTOP-RECEIPT.json")]


def normalized(path: str):
    return os.path.normcase(os.path.normpath(path))


def qualify(positive: Path, negative: Path, binary: Path):
    actual, actual_receipt, evidence, raw_actual = checked_run(positive, False)
    control, control_receipt, _, raw_control = checked_run(negative, True)
    for field in ("fixture_sha256", "runtime_sha256", "sdk_commit", "source_files"):
        assert actual[field] == control[field], "Positive and negative inputs differ"
    native = digest(binary / "p9-dlss-d3d11-adapter.exe")
    snippet = digest(binary / "nvngx_dlss.dll")
    assert native["sha256"] == actual["fixture_sha256"] and snippet["sha256"] == actual["runtime_sha256"]
    log = (positive / "Desktop" / "stderr.log").read_text(encoding="utf-8-sig")
    loaded = re.findall(r"Loaded library from path \(([^\r\n]+)\)", log)
    assert any(normalized(p) == normalized(snippet["path"]) for p in loaded), "Pinned SDK snippet not loaded"
    assert re.search(r"Snippet version\s*:\s*310\.9\.1(?:,|\s)", log)
    drivers = set(re.findall(r"Driver version\s*:\s*([\d.]+)", log))
    assert len(drivers) == 1
    models = sorted({p for p in loaded if p.lower().endswith(".bin")})
    assert models, "No actual model provenance in SDK callback"
    # Hash the installed files in place. Never duplicate driver/model caches.
    core_paths = {r["actual_ngx_core"] for r in evidence if r["check"] == "real_feature_created"}
    assert len(core_paths) == 1
    core = digest(Path(next(iter(core_paths))))
    model_files = [digest(Path(path)) for path in models]
    return {
        "schema": 1, "qualified_utc": datetime.now(timezone.utc).isoformat(),
        "qualification": "REAL_DLSS_SHARED_TEXTURE_ADAPTER_NATIVE_PROOF",
        "source_inputs": actual["source_files"], "sdk_commit": actual["sdk_commit"],
        "binary": native, "configured_and_loaded_sdk_snippet": snippet,
        "observed_ngx_core": core, "observed_model_files": model_files,
        "provenance_limit": "Post-run hashes of callback-identified installed core/model files; not a loader-time attestation",
        "driver_version": next(iter(drivers)), "actual_evaluations": 12, "retired_generations": 3,
        "negative_control": "Untouched output rejected by the same real output validator; zero evaluations",
        "positive_run": str(positive.resolve()), "negative_run": str(negative.resolve()),
        "private_desktops": [actual_receipt["desktop"], control_receipt["desktop"]],
        "input_desktop_unchanged": True, "visible_input_desktop_windows": [],
        "raw_evidence": raw_actual + raw_control,
        "limits": [
            "Small synthetic inputs, zero motion/jitter and controlled exposure; not dense game coverage.",
            "Costs include synthetic uploads, completion observation and readback; not gameplay or isolated GPU timing.",
            "No in-game DLSS/jitter/frame generation enabled and no performance promotion.",
            "Signed runtime input validated by the launcher; SDK source or compilation alone is not runtime proof."
        ],
        "next_gate": "Owned exact-frame game inputs and complete motion coverage, then synchronous draw-owner fallback integration."
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--positive", type=Path, required=True)
    parser.add_argument("--negative", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior qualification"
    result = qualify(args.positive, args.negative, args.binary)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in (
        "qualification", "actual_evaluations", "retired_generations", "driver_version", "next_gate")}, indent=2))
