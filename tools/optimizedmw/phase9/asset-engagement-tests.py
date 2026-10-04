#!/usr/bin/env python3
"""Focused false-positive/partial-capture regressions for the release gate."""
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location("engagement", pathlib.Path(__file__).with_name("verify-asset-engagement.py"))
QC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QC)
SOURCE = "e" * 40
EXE = "7" * 64


def lane_values(lane):
    data = {key: 0 for key in QC.REQUIRED[lane]}
    data.update(requested=12, eligible=4)
    if lane == "L1":
        data.update(built=2, original_indices=600, derived_indices=300, consumed_draws=8, consumed_indices=1200)
    elif lane == "H1":
        data.update(built=1, control_submissions=3, proxy_submissions=1, consumed_draws=8, consumed_indices=1200)
    elif lane == "T1":
        data.update(ready=3, native_images=2, consumed_main_draws=8)
    else:
        data.update(boundaries=45, allocations=2, uploads=10, upload_bytes=8000,
                    coarse_published=1, fine_published=1, coarse_draws=8, fine_draws=4,
                    live_bytes=8000, peak_bytes=12000)
    return data


def format_values(values):
    return " ".join(f"{key}={value}" for key, value in values.items())


def proof(mask, changes=None, omit_refusal=None):
    lines = [f"[00:00] OptimizedMW asset QC v1: final=1 mask={mask} draw_stopped=1 worker_frozen=0"]
    for bit, lane in QC.LANES.items():
        if mask & bit:
            values = lane_values(lane)
            values.update((changes or {}).get(lane, {}))
            if mask & 16 and lane == "T2":
                values.update(demotion_eligible=1, demotions=1, demotion_draws=2)
            lines.append(f"[00:00] OptimizedMW asset QC {lane}: " + format_values(values))
            if omit_refusal != lane:
                lines.append(f"[00:00] OptimizedMW asset QC {lane} refusals: unsupported=0 admission=0 not_ready=0")
    return "\n".join(lines) + "\n"


def members(mask, log=None, exit_code=0):
    manifest = {"engine_source_commit_from_build_identity": SOURCE, "executable": {"sha256": EXE},
                "asset_pipeline": {"mask": mask}, "phase9_mode": {"AssetMask": mask},
                "launcher_applied_openmw_environment": {"OPENMW_P9_ASSET_MASK": str(mask)},
                "completion": {"exit_code": exit_code}}
    report = {"gameplay_context": {"qualified": True, "observed_state_cohorts": {"cohorts": {
        "observed_active": {"frames": 2}}}}}
    return {"RUN-MANIFEST.json": json.dumps(manifest), "openmw.log": proof(mask) if log is None else log,
            "Phase9-BENCHMARK-REPORT.json": json.dumps(report),
            "benchmark-context.csv": "frame,observed_flags\n1,4\n2,0\n3,0\n",
            "v3-frame.csv": "frame,wall_ms\n1,15.2\n2,16.7\n3,18.2\n",
            "benchmark-context.csv.capture-status.txt": "schema=1\nnormal_finish=1\noutput_ok=1\nallocation_failed=0\n"
                "dropped=0\nsnapshots_dropped=0\nunfinished_frames=0\nrows=3\n"}


class EngagementTests(unittest.TestCase):
    def evaluate(self, data, zip_input=True, **kw):
        with tempfile.TemporaryDirectory(prefix="p9-engagement-") as temporary:
            root = pathlib.Path(temporary)
            if zip_input:
                path = root / "capture.zip"
                with zipfile.ZipFile(path, "w") as z:
                    for name, value in data.items():
                        z.writestr(name, value)
            else:
                path = root / "capture"
                path.mkdir()
                for name, value in data.items():
                    (path / name).write_text(value, encoding="utf-8")
            return QC.verify_capture(path, **kw)

    def test_positive_each_lane_and_combination(self):
        for mask in (1, 2, 4, 8, 15, 24, 31):
            with self.subTest(mask=mask):
                result = self.evaluate(members(mask), expected_source=SOURCE, expected_exe=EXE)
                self.assertEqual(result["exit_code"], 0, result)
                self.assertTrue(result["actually_engaged"])
                self.assertEqual(result["visual_acceptance"], "NOT_ASSESSED")
                self.assertEqual(result["performance_acceptance"], "NOT_ASSESSED")

    def test_directory_matches_zip(self):
        for zip_input in (False, True):
            self.assertEqual(self.evaluate(members(1), zip_input)["status"], "ENGAGEMENT_QUALIFIED")

    def test_control_has_no_engagement_claim(self):
        result = self.evaluate(members(0, log="old control ordinary output\n"))
        self.assertEqual(result["status"], "CONTROL_REFERENCE")
        self.assertFalse(result["actually_engaged"])

    def test_prepared_built_never_means_drawn(self):
        for lane, mask in (("L1", 1), ("H1", 2), ("T1", 4), ("T2", 8)):
            changes = {"consumed_draws": 0, "consumed_indices": 0, "consumed_main_draws": 0, "coarse_draws": 0, "fine_draws": 0}
            result = self.evaluate(members(mask, proof(mask, {lane: changes})))
            self.assertEqual(result["exit_code"], 2, result)
            self.assertEqual(result["lanes"][lane]["status"], "PREPARED_NOT_CONSUMED")
            self.assertFalse(result["actually_engaged"])

    def test_noeligible_distinct_from_defective_pipeline(self):
        for lane, mask in (("L1", 1), ("H1", 2), ("T1", 4), ("T2", 8)):
            zeros = {key: 0 for key in QC.REQUIRED[lane]}
            zeros["requested"] = 3
            result = self.evaluate(members(mask, proof(mask, {lane: zeros})))
            self.assertEqual(result["exit_code"], 2)
            self.assertEqual(result["lanes"][lane]["status"], "NO_ELIGIBLE_WORK")

    def test_eligible_but_not_built_separate(self):
        values = {key: 0 for key in QC.REQUIRED["L1"]}
        values.update(requested=3, eligible=2)
        result = self.evaluate(members(1, proof(1, {"L1": values})))
        self.assertEqual(result["lanes"]["L1"]["status"], "ELIGIBLE_NOT_PREPARED")

    def test_outer_admission_blocks_eligibility_without_natural_ineligible_claim(self):
        for lane, mask in (("L1", 1), ("H1", 2)):
            for requested in (0, 3):
                with self.subTest(lane=lane, requested=requested):
                    zeros = {key: 0 for key in QC.REQUIRED[lane]}
                    zeros["requested"] = requested
                    log = proof(mask, {lane: zeros}).replace("refusals: unsupported=0",
                        "refusals: outer_cpu_denied=2 source_rejected=0 unsupported=0")
                    result = self.evaluate(members(mask, log))
                    self.assertEqual(result["exit_code"], 2)
                    self.assertFalse(result["actually_engaged"])
                    self.assertEqual(result["lanes"][lane]["status"], "ELIGIBILITY_BLOCKED")
                    self.assertEqual(result["lanes"][lane]["eligibility_evidence"],
                        "ADMISSION_BLOCKED_BEFORE_SOURCE_CERTIFICATION")
                    self.assertIn("outer CPU admission", result["lanes"][lane]["problems"][0])
                    self.assertIn("ELIGIBILITY_BLOCKED", QC.report_text(result))

    def test_zero_outer_denial_does_not_invent_blocked_eligibility(self):
        zeros = {key: 0 for key in QC.REQUIRED["L1"]}
        zeros["requested"] = 3
        log = proof(1, {"L1": zeros}).replace("refusals: unsupported=0",
            "refusals: outer_cpu_denied=0 source_rejected=0 unsupported=0")
        result = self.evaluate(members(1, log))
        self.assertEqual(result["exit_code"], 2)
        self.assertEqual(result["lanes"]["L1"]["status"], "NO_ELIGIBLE_WORK")
        self.assertEqual(result["lanes"]["L1"]["eligibility_evidence"],
            "SOURCE_REQUESTS_WITHOUT_CERTIFICATION_PROOF")

    def test_missing_refusal_cannot_default_zero(self):
        result = self.evaluate(members(1, proof(1, omit_refusal="L1")))
        self.assertEqual(result["exit_code"], 3)
        self.assertFalse(result["actually_engaged"])

    def test_old_summary_and_optional_diagnostics_cannot_pass(self):
        log = "OptimizedMW asset pipeline: mask=1 lod_built=8 lod_derived_draws=9\n"
        result = self.evaluate(members(1, log))
        self.assertEqual(result["exit_code"], 3)
        self.assertTrue(result["historical_partial_summaries"])

    def test_crash_overrides_positive_or_missing_counters(self):
        for log in (proof(8), ""):
            data = members(8, log, exit_code=1)
            data.pop("v3-frame.csv")
            result = self.evaluate(data)
            self.assertEqual(result["exit_code"], 4)
            self.assertFalse(result["actually_engaged"])

    def test_dump_overrides_spurious_exit_zero(self):
        data = members(1)
        data["openmw-crash.dmp"] = "partial dump marker"
        self.assertEqual(self.evaluate(data)["exit_code"], 4)

    def test_counter_missing_duplicate_negative_hex_or_float(self):
        good = proof(1)
        for bad in (good.replace("consumed_draws=8 ", ""), good.replace("built=2", "built=2 built=2"),
                    good.replace("built=2", "built=-1"), good.replace("built=2", "built=0x2"),
                    good.replace("built=2", "built=2.0"), good + good.splitlines()[0] + "\n"):
            self.assertEqual(self.evaluate(members(1, bad))["exit_code"], 3)

    def test_actual_mask_final_and_draw_stop_required(self):
        for bad in (proof(1).replace("mask=1", "mask=2"), proof(1).replace("final=1", "final=0"),
                    proof(1).replace("draw_stopped=1", "draw_stopped=0"), proof(1).replace("worker_frozen=0", "")):
            self.assertEqual(self.evaluate(members(1, bad))["exit_code"], 3)

    def test_nontransactional_worker_counters_no_false_conservation(self):
        log = proof(1, {"L1": {"requested": 3, "eligible": 4}})
        self.assertEqual(self.evaluate(members(1, log))["exit_code"], 0)
        frozen = log.replace("worker_frozen=0", "worker_frozen=1")
        self.assertEqual(self.evaluate(members(1, frozen))["exit_code"], 3)

    def test_structural_reduction_is_required(self):
        for mask, lane, bad in ((1, "L1", {"derived_indices": 600}),
                                (2, "H1", {"proxy_submissions": 3})):
            self.assertEqual(self.evaluate(members(mask, proof(mask, {lane: bad})))["exit_code"], 3)

    def test_native_apply_is_not_draw_and_purpose_is_explicit(self):
        log = proof(4, {"T1": {"consumed_main_draws": 0, "full_applies": 20, "consumed_composite_draws": 0}})
        result = self.evaluate(members(4, log))
        self.assertEqual(result["exit_code"], 2)
        self.assertFalse(result["lanes"]["T1"]["actually_engaged"])
        composite = proof(4, {"T1": {"consumed_main_draws": 0, "consumed_composite_draws": 50}})
        result = self.evaluate(members(4, composite))
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["lanes"]["T1"]["status"], "ENGAGED_COMPOSITE_SOURCE_ONLY")
        self.assertEqual(result["lanes"]["T1"]["consumer_scope"], "COMPOSITE_SOURCE_ONLY")

    def test_texture_draw_requires_publication_upload_and_boundary(self):
        for key in ("coarse_published", "allocations", "uploads", "upload_bytes", "boundaries"):
            result = self.evaluate(members(8, proof(8, {"T2": {key: 0}})))
            self.assertEqual(result["exit_code"], 3, key)

    def test_initial_coarse_draw_not_demotion_receipt(self):
        log = proof(24).replace("demotion_draws=2", "demotion_draws=0")
        result = self.evaluate(members(24, log))
        self.assertEqual(result["exit_code"], 2)
        self.assertTrue(result["lanes"]["T2"]["actually_engaged"])
        self.assertFalse(result["lanes"]["DEMOTION"]["actually_engaged"])

    def test_missing_state_bad_writer_or_noactive_does_not_pass(self):
        for name, edit in (("benchmark-context.csv", lambda x: x.replace("3,0\n", "")),
                           ("benchmark-context.csv.capture-status.txt", lambda x: x.replace("dropped=0", "dropped=1")),
                           ("benchmark-context.csv", lambda x: x.replace(",0\n", ",32\n"))):
            data = members(1)
            data[name] = edit(data[name])
            self.assertEqual(self.evaluate(data)["exit_code"], 3)

    def test_exact_release_identity_required(self):
        self.assertEqual(self.evaluate(members(1), expected_source="a" * 40)["exit_code"], 3)
        self.assertEqual(self.evaluate(members(1), expected_exe="a" * 64)["exit_code"], 3)

    def test_raw_manifest_hash_preserves_bom(self):
        data = members(1)
        data["RUN-MANIFEST.json"] = "\ufeff" + data["RUN-MANIFEST.json"]
        result = self.evaluate(data)
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["manifest_sha256"], hashlib.sha256(data["RUN-MANIFEST.json"].encode()).hexdigest())

    def test_malformed_json_cannot_overwrite_or_escape_report(self):
        for invalid in ('[]', '{"asset_pipeline": {}, "asset_pipeline": {"mask": 1}}', '{"value": NaN}'):
            data = members(1)
            data["RUN-MANIFEST.json"] = invalid
            self.assertEqual(self.evaluate(data)["exit_code"], 3)

    def test_unused_empty_report_cohort_nan_is_not_active_proof(self):
        data = members(1)
        report = json.loads(data["Phase9-BENCHMARK-REPORT.json"])
        report["unrelated_empty_cohort"] = {"mean": float("nan")}
        data["Phase9-BENCHMARK-REPORT.json"] = json.dumps(report)
        self.assertEqual(self.evaluate(data)["exit_code"], 0)
        report["gameplay_context"]["observed_state_cohorts"]["cohorts"]["observed_active"]["frames"] = float("nan")
        data["Phase9-BENCHMARK-REPORT.json"] = json.dumps(report)
        self.assertEqual(self.evaluate(data)["exit_code"], 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
