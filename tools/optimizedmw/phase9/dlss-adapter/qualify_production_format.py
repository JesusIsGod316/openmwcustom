"""Qualify finished import-only packed-depth/OSG evidence; never launch a process."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path
import struct

from qualify_adapter import digest, normalized, read_json, records


def validate_rows(evidence: list[dict], stdout: list[dict], negative: bool):
    assert all(row.get("status") == "pass" for row in evidence + stdout), "Native failure marker"
    assert all(row.get("actual_dlss_evaluations", 0) == 0 for row in evidence + stdout), "Import-only proof evaluated NGX"
    assert not any(row.get("game_dlss_enabled") or row.get("dense_game_motion_proved")
                   or row.get("performance_promoted") for row in evidence + stdout), "Unproved production claim"
    device = [r for r in evidence if r["check"] == "production_format_device"]
    assert len(device) == 1
    device = device[0]
    assert all(device[k] for k in ("same_physical_device", "nvidia_vendor", "window_hidden"))
    assert device["osg_version"] == "3.6.5" and device["device_node_mask"] > 0
    assert "NVIDIA" in device["gl_renderer"] and device["gl_version"]
    imports = [r for r in evidence if r["check"] == "production_format_import"]
    completion = [r for r in evidence if r["check"] == "production_format_complete"]
    negative_markers = [r for r in stdout if r["check"] == "production_format_negative"]
    if negative:
        assert not imports and not completion, "Corruption control must fail before a qualified frame"
        assert len(negative_markers) == 1 and negative_markers[0]["corrupted_import_rejected"]
    else:
        assert not negative_markers and len(completion) == 1
        assert completion[0]["copied_frames"] == 16 and completion[0]["ordinary_geometry_draws"] == 32
        assert completion[0]["retired_generations"] == 4
        assert len(imports) == 16
        assert [(r["sequence"], r["generation"]) for r in imports] == [(i + 1, i // 4 + 1) for i in range(16)]
        profiles = [("DEPTH24_STENCIL8", False), ("DEPTH24_STENCIL8", True),
                    ("DEPTH32F_STENCIL8", False), ("DEPTH32F_STENCIL8", True)]
        for i, row in enumerate(imports):
            assert row["color_format"] == "RGBA8" and (row["width"], row["height"]) == (64, 48)
            assert (row["depth_format"], row["reversed_depth"]) == profiles[i // 4]
            assert all(row[k] is True for k in ("exact_pixels", "source_stencil_unchanged",
                "borrowed_compare_unchanged", "owned_noncomparison_sampler", "raw_state_restored",
                "osg_cache_restored", "ordinary_geometry_exact", "fallback_untouched_on_refusal",
                "independent_d3d_pixels_exact", "packed_source_representation_unchanged",
                "depth_expectation_independent_of_import")), "Incomplete actual-format import or following-draw proof"
            assert row["depth_mapping"] == "D24_uint_div_16777215_round_once_to_R32F_or_D32F_identity"
            assert row["import_depth_allowed_ulps"] == 0 and row["maximum_cpu_depth_readback_ulps"] <= 1
            canonical = row["depth_canonical_float_bits_by_quadrant"]
            assert len(canonical) == 4 and canonical == row["depth_import_float_bits_by_quadrant"]
            cpu = row["depth_cpu_float_bits_by_quadrant"]
            assert len(cpu) == 4 and all(abs(a-b) <= 1 for a, b in zip(cpu, canonical))
            if row["depth_format"] == "DEPTH24_STENCIL8":
                integers = row["depth_packed_integers_by_quadrant"]
                assert len(integers) == 4 and not row["depth_packed_float_bits_by_quadrant"]
                assert all(isinstance(v, int) and 0 <= v <= 16777215 for v in integers)
                expected = [struct.unpack("<I", struct.pack("<f", v / 16777215.0))[0] for v in integers]
                assert canonical == expected, "R32 depth expectation is not the independent packed-integer mapping"
            else:
                assert not row["depth_packed_integers_by_quadrant"]
                assert canonical == row["depth_packed_float_bits_by_quadrant"] == cpu
    return device


def checked_run(directory: Path, negative: bool):
    inputs = read_json(directory / "INPUTS.json")
    receipt = read_json(directory / "Desktop/DESKTOP-RECEIPT.json")
    assert inputs["negative_control"] is negative and inputs["profile_mutex_held"] is True
    assert inputs["production_format_fixture"] is True and inputs["game_package_changed"] is False
    assert receipt["state"] == "exited" and receipt["exit_code"] == 0 and not receipt["timed_out"]
    assert not receipt["desktop_switched"] and receipt["input_desktop_unchanged"]
    assert not receipt["input_desktop_owned_visible_windows"], "Visible native fixture window"
    sources = inputs["source_files"]
    assert len({normalized(s["path"]) for s in sources}) == len(sources), "Duplicate source identity"
    assert {"production-format-tests.cpp", "source-textures.hpp", "borrowed-gl-state.hpp", "gpu-transfers.hpp",
        "shared-d3d11-textures.hpp", "synchronousinputlease.hpp", "CMakeLists.txt"} <= {Path(s["path"]).name for s in sources}
    for item in sources + inputs["osg_runtime_files"]:
        actual = digest(Path(item["path"]))
        assert actual["bytes"] == item["bytes"] and actual["sha256"] == item["sha256"], "Native input changed"
    pin_path = Path(inputs["compiled_binary_pin"])
    assert digest(pin_path)["sha256"] == inputs["compiled_binary_pin_sha256"], "Immutable binary pin changed"
    pin = read_json(pin_path)
    assert pin["source_files"] == sources, "Native source differs from compilation pin"
    pin_files = {normalized(i["path"]): i["sha256"] for i in pin["binary_files"]}
    assert pin_files[normalized(inputs["fixture_path"])] == inputs["fixture_sha256"]
    for item in inputs["osg_runtime_files"]:
        assert pin_files[normalized(item["path"])] == item["sha256"], "OSG runtime absent from compiled binary pin"
    evidence = records(directory / "format-evidence.jsonl")
    stdout = records(directory / "Desktop/stdout.log")
    device = validate_rows(evidence, stdout, negative)
    runtime_paths = {normalized(i["path"]) for i in inputs["osg_runtime_files"]}
    assert normalized(device["osg_runtime_path"]) in runtime_paths, "Actual OSG DLL differs from pin"
    assert normalized(device["openthreads_runtime_path"]) in runtime_paths, "Actual OpenThreads DLL differs from pin"
    raw = [digest(directory / p) for p in ("INPUTS.json", "format-evidence.jsonl", "Desktop/stdout.log",
        "Desktop/stderr.log", "Desktop/DESKTOP-RECEIPT.json")]
    return inputs, receipt, device, raw


def qualify(positive: Path, negative: Path, binary_path: Path):
    actual, actual_receipt, device, actual_raw = checked_run(positive, False)
    control, control_receipt, _, control_raw = checked_run(negative, True)
    for key in ("fixture_path", "fixture_sha256", "source_files", "osg_runtime_files",
                "compiled_binary_pin", "compiled_binary_pin_sha256"):
        assert actual[key] == control[key], "Positive and corruption control have different native inputs"
    assert normalized(str(binary_path.resolve())) == normalized(actual["fixture_path"])
    assert digest(binary_path)["sha256"] == actual["fixture_sha256"], "Qualified executable changed"
    return {
        "schema": 1, "qualified_utc": datetime.now(timezone.utc).isoformat(),
        "qualification": "PACKED_DEPTH_IMPORT_OSG_NATIVE_PROOF", "actual_imports": 16,
        "ordinary_textured_geometry_draws": 32, "retired_generations": 4,
        "depth_profiles": ["DEPTH24_STENCIL8", "DEPTH32F_STENCIL8"],
        "ordinary_and_reversed_depth": True, "owned_noncomparison_sampler": True,
        "independent_d3d_pixels_exact": True, "corruption_negative_rejected": True,
        "osg_version": device["osg_version"], "gl_renderer": device["gl_renderer"], "gl_version": device["gl_version"],
        "source_inputs": actual["source_files"], "fixture": digest(binary_path),
        "osg_runtime_files": actual["osg_runtime_files"], "raw_positive": actual_raw, "raw_negative": control_raw,
        "positive_desktop": actual_receipt, "negative_desktop": control_receipt,
        "actual_dlss_evaluations": 0, "game_dlss_enabled": False, "dense_game_motion_proved": False,
        "performance_promoted": False,
        "scope": "Controlled synthetic GL owner; production-like realized formats, strict synchronous lease, bounded OSG/raw state interleave",
        "next_gate": "Actual game input identities, color/depth alignment and full dense motion closure before production evaluation",
    }


if __name__ == "__main__":
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--positive", type=Path, required=True)
    parser.add_argument("--negative", type=Path, required=True)
    parser.add_argument("--binary-path", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior qualification"
    result = qualify(args.positive, args.negative, args.binary_path)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("qualification", "actual_imports",
        "ordinary_textured_geometry_draws", "actual_dlss_evaluations", "next_gate")}, indent=2))
