"""Rejection tests for import-only format evidence; no graphics or processes."""
import copy
from pathlib import Path
import tempfile
import unittest
import json
import struct

from qualify_production_format import validate_rows, qualify, digest


def rows(negative=False):
    device = dict(schema=1, check="production_format_device", status="pass", same_physical_device=True,
        nvidia_vendor=True, window_hidden=True, osg_version="3.6.5", device_node_mask=1,
        gl_renderer="NVIDIA controlled fixture", gl_version="4.6 test", osg_runtime_path="osg.dll",
        openthreads_runtime_path="OpenThreads.dll", actual_dlss_evaluations=0,
        game_dlss_enabled=False, dense_game_motion_proved=False)
    if negative:
        return [device], [dict(schema=1, check="production_format_negative", status="pass",
            corrupted_import_rejected=True, actual_dlss_evaluations=0)]
    result = [device]
    profiles = [("DEPTH24_STENCIL8", False), ("DEPTH24_STENCIL8", True),
                ("DEPTH32F_STENCIL8", False), ("DEPTH32F_STENCIL8", True)]
    for i in range(16):
        depth, reverse = profiles[i // 4]
        values = [1-v if reverse else v for v in (.25, .375, .5, .75)]
        packed = [round(v*16777215) for v in values] if depth == "DEPTH24_STENCIL8" else []
        bits = [struct.unpack("<I", struct.pack("<f", v))[0] for v in (
            [v/16777215.0 for v in packed] if packed else values)]
        result.append(dict(schema=1, check="production_format_import", status="pass", sequence=i+1,
            generation=i//4+1, color_format="RGBA8", depth_format=depth, reversed_depth=reverse,
            width=64, height=48, exact_pixels=True, source_stencil_unchanged=True,
            borrowed_compare_unchanged=True, owned_noncomparison_sampler=True, raw_state_restored=True,
            osg_cache_restored=True, ordinary_geometry_exact=True, fallback_untouched_on_refusal=True,
            independent_d3d_pixels_exact=True, packed_source_representation_unchanged=True,
            depth_expectation_independent_of_import=True,
            depth_mapping="D24_uint_div_16777215_round_once_to_R32F_or_D32F_identity",
            import_depth_allowed_ulps=0, maximum_cpu_depth_readback_ulps=1,
            depth_packed_integers_by_quadrant=packed, depth_packed_float_bits_by_quadrant=[] if packed else bits,
            depth_cpu_float_bits_by_quadrant=bits, depth_canonical_float_bits_by_quadrant=bits,
            depth_import_float_bits_by_quadrant=bits,
            actual_dlss_evaluations=0, dense_game_motion_proved=False))
    result.append(dict(schema=1, check="production_format_complete", status="pass", copied_frames=16,
        ordinary_geometry_draws=32, retired_generations=4, actual_dlss_evaluations=0,
        game_dlss_enabled=False, dense_game_motion_proved=False, performance_promoted=False))
    return result, copy.deepcopy(result)


class RowRejection(unittest.TestCase):
    def test_valid_controlled_shapes(self):
        validate_rows(*rows(), False)
        validate_rows(*rows(True), True)

    def test_all_native_pixel_state_and_fallback_proofs_required(self):
        for key in ("exact_pixels", "source_stencil_unchanged", "borrowed_compare_unchanged",
            "owned_noncomparison_sampler", "raw_state_restored", "osg_cache_restored", "ordinary_geometry_exact",
            "fallback_untouched_on_refusal", "independent_d3d_pixels_exact",
            "packed_source_representation_unchanged", "depth_expectation_independent_of_import"):
            with self.subTest(key=key):
                evidence, stdout = rows(); evidence[1][key] = False
                with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_import_depth_expectation_and_comparison_remain_exact(self):
        for key, value in (("depth_mapping", "observed_import"), ("import_depth_allowed_ulps", 1),
                           ("maximum_cpu_depth_readback_ulps", 2)):
            evidence, stdout = rows(); evidence[1][key] = value
            with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_packed_integer_math_is_independent_and_bit_exact(self):
        evidence, stdout = rows(); evidence[1]["depth_packed_integers_by_quadrant"][1] += 1
        with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)
        evidence, stdout = rows(); evidence[1]["depth_import_float_bits_by_quadrant"] = [0]*4
        with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_exact_realized_profiles_and_depth_conventions_required(self):
        for key, value in (("depth_format", "DEPTH_COMPONENT32F"), ("reversed_depth", True),
            ("color_format", "RGBA16F"), ("width", 65), ("generation", 2), ("sequence", 2)):
            with self.subTest(key=key):
                evidence, stdout = rows(); evidence[1][key] = value
                with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_missing_duplicated_frames_rejected(self):
        for replacement in (lambda e: e[:-2]+e[-1:], lambda e: e[:2]+e[1:]):
            evidence, stdout = rows()
            with self.assertRaises(AssertionError): validate_rows(replacement(evidence), stdout, False)

    def test_inflated_completion_rejected(self):
        for key in ("copied_frames", "ordinary_geometry_draws", "retired_generations"):
            evidence, stdout = rows(); evidence[-1][key] += 1
            with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_import_proof_cannot_claim_ngx_game_dense_or_performance(self):
        for key in ("actual_dlss_evaluations", "game_dlss_enabled", "dense_game_motion_proved", "performance_promoted"):
            evidence, stdout = rows(); evidence[-1][key] = True
            with self.assertRaises(AssertionError): validate_rows(evidence, stdout, False)

    def test_corruption_control_must_reject_before_qualified_import(self):
        evidence, stdout = rows(True); evidence.append(rows()[0][1])
        with self.assertRaises(AssertionError): validate_rows(evidence, stdout, True)

    def test_unobserved_negative_is_not_native_success(self):
        evidence, stdout = rows(True)
        with self.assertRaises(AssertionError): validate_rows(evidence, [], True)
        stdout[0]["corrupted_import_rejected"] = False
        with self.assertRaises(AssertionError): validate_rows(evidence, stdout, True)


class ProvenanceRejection(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / "p9-dlss-production-format.exe"; self.binary.write_bytes(b"native fixture pin")
        self.runtime = []
        for name in ("osg.dll", "OpenThreads.dll"):
            path = self.root / name; path.write_bytes(name.encode()); self.runtime.append(digest(path))
        self.sources = []
        for name in ("production-format-tests.cpp", "source-textures.hpp", "borrowed-gl-state.hpp", "gpu-transfers.hpp",
                     "shared-d3d11-textures.hpp", "synchronousinputlease.hpp", "CMakeLists.txt"):
            path = self.root / name; path.write_text(name); self.sources.append(digest(path))
        pin = dict(source_files=self.sources, binary_files=[digest(self.binary)]+self.runtime)
        self.pin = self.root / "BINARY-PIN.json"; self.pin.write_text(json.dumps(pin))
        self.positive = self.root / "positive"; self.negative = self.root / "negative"
        for directory, negative in ((self.positive, False), (self.negative, True)):
            (directory / "Desktop").mkdir(parents=True)
            inputs = dict(negative_control=negative, profile_mutex_held=True, production_format_fixture=True,
                game_package_changed=False, source_files=self.sources, osg_runtime_files=self.runtime,
                compiled_binary_pin=str(self.pin), compiled_binary_pin_sha256=digest(self.pin)["sha256"],
                fixture_path=str(self.binary), fixture_sha256=digest(self.binary)["sha256"])
            (directory / "INPUTS.json").write_text(json.dumps(inputs))
            receipt = dict(state="exited", exit_code=0, timed_out=False, desktop_switched=False,
                input_desktop_unchanged=True, input_desktop_owned_visible_windows=[])
            (directory / "Desktop/DESKTOP-RECEIPT.json").write_text(json.dumps(receipt))
            evidence, stdout = rows(negative)
            for row in [evidence[0], *([stdout[0]] if not negative else [])]:
                row["osg_runtime_path"] = str(self.root / "osg.dll")
                row["openthreads_runtime_path"] = str(self.root / "OpenThreads.dll")
            for name, content in (("format-evidence.jsonl", evidence), ("Desktop/stdout.log", stdout)):
                (directory / name).write_text("\n".join(json.dumps(r) for r in content)+"\n")
            (directory / "Desktop/stderr.log").write_text("")

    def test_exact_pinned_pair(self):
        proof = qualify(self.positive, self.negative, self.binary)
        self.assertEqual(proof["actual_imports"], 16)
        self.assertEqual(proof["actual_dlss_evaluations"], 0)

    def test_changed_current_source_rejected(self):
        Path(self.sources[0]["path"]).write_text("changed")
        with self.assertRaises(AssertionError): qualify(self.positive, self.negative, self.binary)

    def test_changed_runtime_rejected(self):
        Path(self.runtime[0]["path"]).write_bytes(b"changed")
        with self.assertRaises(AssertionError): qualify(self.positive, self.negative, self.binary)

    def test_changed_compilation_pin_rejected(self):
        self.pin.write_text(self.pin.read_text()+" ")
        with self.assertRaises(AssertionError): qualify(self.positive, self.negative, self.binary)

    def test_visible_native_window_rejected(self):
        path = self.positive / "Desktop/DESKTOP-RECEIPT.json"
        receipt = json.loads(path.read_text()); receipt["input_desktop_owned_visible_windows"] = [123]
        path.write_text(json.dumps(receipt))
        with self.assertRaises(AssertionError): qualify(self.positive, self.negative, self.binary)

    def test_substituted_actual_osg_module_rejected(self):
        path = self.positive / "format-evidence.jsonl"
        evidence = [json.loads(line) for line in path.read_text().splitlines()]
        evidence[0]["osg_runtime_path"] = str(self.root / "other-osg.dll")
        path.write_text("\n".join(json.dumps(r) for r in evidence))
        with self.assertRaises(AssertionError): qualify(self.positive, self.negative, self.binary)


if __name__ == "__main__":
    unittest.main()
