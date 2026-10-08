"""Regression checks for false-positive asset package qualification evidence."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("asset_release", Path(__file__).with_name("verify-asset-shader-release.py"))
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class AssetReleaseEvidence(unittest.TestCase):
    @staticmethod
    def post_optimizer_receipt():
        return ("CHAIN_POST_OPTIMIZER baseline_submissions=2 retained_submissions=2 contributor_submissions=4 "
                "exact_control_indices=1 private_array_copies=0 private_vertex_buffers=0 "
                "gpu_debt_bytes=4096 new_index_bytes=4096 merged_input_population=1 "
                "weak_geometry_unchanged=1 source_geometry_unchanged=1 actual_producer=1\n"
                "CHAIN_POST_OPTIMIZER_BUFFERS shared_stream_geometries=2 shared_all_arrays_vbos=1 "
                "unchanged_primitive_ebo_identity=1 established_control_vbos=1 actual_main_draw=1\n"
                "CHAIN_SHADER_FEATURES immutable_index_streams=2 actual_program_linked=1\n"
                "CHAIN_MIXED_UNAPPROVED_REFUSAL unsupported_record_kind=1 same_template_same_material=1 "
                "whole_merged_draw_refused=1 build_attempted=0 exact_control_batching=1 exact_control_indices=1 "
                "contributor_submissions=4 baseline_submissions=2 merged_input_population=1\n"
                "CHAIN_POST_OPTIMIZER_VBO_REFUSAL display_list_refused=1 missing_vbo_refused=1 new_gpu_debt=0 private_copy_only=1\n"
                "CHAIN_NONMERGE_SHARED_TEMPLATE actual_borrowed_drawable=1 template_unchanged=1 weak_unchanged=1 "
                "no_derived_tier=1 new_gpu_debt=0 negative_only_merge_override=1\n")

    def test_actual_contract_population(self):
        text = "ASSET_CONTRACT_PASS stock=1 pbr=1 source_negative=1\n"
        self.assertEqual(gate.record(text, "ASSET_CONTRACT_PASS", {"stock": 1, "pbr": 1})["source_negative"], "1")
        current = "\n".join("ASSET_CONTRACT_REFRESH family=" + family + " cached=1 l1_source=1" for family in ("stock", "pbr"))
        self.assertEqual(set(gate.population(current, "ASSET_CONTRACT_REFRESH", "family", {"stock", "pbr"}, {"cached": 1, "l1_source": 1})), {"stock", "pbr"})
        for bad in ("", current.splitlines()[0], current.replace("family=pbr", "family=stock"), current.replace("l1_source=1", "l1_source=0")):
            with self.subTest(bad=bad), self.assertRaises(gate.QualificationError):
                gate.population(bad, "ASSET_CONTRACT_REFRESH", "family", {"stock", "pbr"}, {"cached": 1, "l1_source": 1})

    def test_zero_engagement_is_not_pass(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("ASSET_MATERIAL_PASS l1=0 h1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1, "h1": 1})
        valid = self.post_optimizer_receipt()
        gate.l1_post_optimizer(valid)
        for before, after in (("merged_input_population=1", "merged_input_population=0"),
                              ("contributor_submissions=4", "contributor_submissions=2"),
                              ("retained_submissions=2", "retained_submissions=3"),
                              ("gpu_debt_bytes=4096", "gpu_debt_bytes=8192"),
                              ("new_index_bytes=4096", "new_index_bytes=0"),
                              ("shared_stream_geometries=2", "shared_stream_geometries=0"),
                              ("immutable_index_streams=2", "immutable_index_streams=3"),
                              ("baseline_submissions=2", "baseline_submissions=18446744073709551616")):
            with self.subTest(after=after), self.assertRaises(gate.QualificationError):
                gate.l1_post_optimizer(valid.replace(before, after))

    def test_admission_wait_requires_renewed_actual_cook(self):
        expected = {"observations": 3, "construction_attempts_before_resume": 0,
                    "ordinary_preserved": 1, "renewed_actual_cook": 1, "generation_reset": 0}
        valid = "CHAIN_RETRY_ADMISSION_WAIT " + " ".join(f"{key}={value}" for key, value in expected.items()) + "\n"
        gate.record(valid, "CHAIN_RETRY_ADMISSION_WAIT", expected)
        for bad in ("CHAIN_RETRY_EXHAUSTED actual_attempts=3 ordinary_preserved=1\n",
                    valid.replace("renewed_actual_cook=1", "renewed_actual_cook=0"),
                    valid.replace("generation_reset=0", "generation_reset=1"),
                    valid.replace("construction_attempts_before_resume=0", "construction_attempts_before_resume=3")):
            with self.subTest(receipt=bad), self.assertRaises(gate.QualificationError):
                gate.record(bad, "CHAIN_RETRY_ADMISSION_WAIT", expected)

    def test_missing_actual_draw_proof(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("ASSET_MATERIAL_PASS l1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1, "draw_fallback": 1})
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_SHADER_FEATURES normal_map_coverage=0 required_h1_normal_map=1\n",
                        "CHAIN_SHADER_FEATURES", {"normal_map_coverage": 1, "required_h1_normal_map": 1})
        valid = self.post_optimizer_receipt()
        for marker in ("CHAIN_POST_OPTIMIZER", "CHAIN_POST_OPTIMIZER_BUFFERS", "CHAIN_SHADER_FEATURES",
                       "CHAIN_MIXED_UNAPPROVED_REFUSAL", "CHAIN_POST_OPTIMIZER_VBO_REFUSAL", "CHAIN_NONMERGE_SHARED_TEMPLATE"):
            missing = "\n".join(line for line in valid.splitlines() if not line.startswith(marker + " ")) + "\n"
            with self.subTest(marker=marker), self.assertRaises(gate.QualificationError):
                gate.l1_post_optimizer(missing)
        for before, after in (("shared_all_arrays_vbos=1", "shared_all_arrays_vbos=0"),
                              ("private_array_copies=0", "private_array_copies=1"),
                              ("weak_geometry_unchanged=1", "weak_geometry_unchanged=0"),
                              ("unchanged_primitive_ebo_identity=1", "unchanged_primitive_ebo_identity=0"),
                              ("actual_main_draw=1", "actual_main_draw=0"),
                              ("contributor_submissions=4", "contributor_submissions=-4"),
                              ("same_template_same_material=1", "same_template_same_material=0"),
                              ("whole_merged_draw_refused=1", "whole_merged_draw_refused=0"),
                              ("build_attempted=0", "build_attempted=1"),
                              ("missing_vbo_refused=1", "missing_vbo_refused=0"),
                              ("new_gpu_debt=0", "new_gpu_debt=4096"),
                              ("template_unchanged=1", "template_unchanged=0"),
                              ("actual_borrowed_drawable=1", "actual_borrowed_drawable=0")):
            with self.subTest(after=after), self.assertRaises(gate.QualificationError):
                gate.l1_post_optimizer(valid.replace(before, after))
        gate.record("CHAIN_L1_TEXTURE_COMBINATION process_mask=5 incremental_control_admission=1 "
                    "post_optimizer=1 private_array_copies=0\n", "CHAIN_L1_TEXTURE_COMBINATION",
                    {"process_mask": 5, "incremental_control_admission": 1, "post_optimizer": 1,
                     "private_array_copies": 0})
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_L1_TEXTURE_COMBINATION process_mask=5 incremental_control_admission=0 "
                        "post_optimizer=1 private_array_copies=0\n", "CHAIN_L1_TEXTURE_COMBINATION",
                        {"process_mask": 5, "incremental_control_admission": 1, "post_optimizer": 1,
                         "private_array_copies": 0})
        index_resources = ("L1_MIXED_INDEX_RESOURCES unchanged_primitive_ebo_identity=1 shared_vertices_vbos=1 "
                           "unchanged_leaves=1 new_index_bytes=4096 exact_new_index_debt=1\n")
        gate.l1_mixed_index_resources(index_resources)
        for bad in ("", index_resources.replace("unchanged_leaves=1", "unchanged_leaves=0"),
                    index_resources.replace("new_index_bytes=4096", "new_index_bytes=0"),
                    index_resources.replace("unchanged_primitive_ebo_identity=1", "unchanged_primitive_ebo_identity=0"),
                    index_resources.replace("exact_new_index_debt=1", "exact_new_index_debt=0"),
                    index_resources.replace("shared_vertices_vbos=1", "shared_vertices_vbos=0")):
            with self.subTest(index_resources=bad), self.assertRaises(gate.QualificationError):
                gate.l1_mixed_index_resources(bad)

    def test_duplicate_terminal_receipts(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_PACKAGE mask=1\nCHAIN_PACKAGE mask=1\n", "CHAIN_PACKAGE", {"mask": 1})
        valid = self.post_optimizer_receipt()
        with self.assertRaises(gate.QualificationError):
            gate.l1_post_optimizer(valid + valid.splitlines()[0] + "\n")

    def test_duplicate_fields(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_PACKAGE mask=0 mask=1\n", "CHAIN_PACKAGE", {"mask": 1})

    def test_skip_cannot_hide_before_positive(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("SKIP: GL unavailable\nASSET_MATERIAL_PASS l1=1\n", "ASSET_MATERIAL_PASS", {"l1": 1})

    def test_mutation_cannot_claim_derived_use(self):
        with self.assertRaises(gate.QualificationError):
            gate.record("CHAIN_SHADER_MUTATION fallback_original=1 derived_draws=1\n", "CHAIN_SHADER_MUTATION", {"fallback_original": 1, "derived_draws": 0})

    def test_changed_input_receipt_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.exe"
            path.write_bytes(b"correct qualified fixture")
            digest = gate.sha(path)
            gate.current_inputs({str(path): digest})
            path.write_bytes(b"changed fixture after validation")
            with self.assertRaises(gate.QualificationError):
                gate.current_inputs({str(path): digest})


if __name__ == "__main__":
    unittest.main()
