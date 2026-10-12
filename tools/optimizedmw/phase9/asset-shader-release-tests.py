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
    def active_grid_receipt():
        return ("CHAIN_ACTIVE_POPULATION candidate_instances=16 ordinary_instances=1 total_instances=17 "
                "spatial_buckets=2 captured_merge_settings=1 source_population_substituted=1 source_loader_substituted=0\n"
                "CHAIN_ACTIVE_PARTIAL_TAIL missing_skipped=1 missing_unpublished=1 weak_exact=1 weak_unchanged=1 "
                "skipped_no_ordinary_build=1 skipped_no_asset_attempt=1 insufficient_normal_exact=1 "
                "wrong_spatial_exact=1 busy_exact=1 available_cooked=1 "
                "incomplete_retry_retained=1 exact_owner_ready=1 source_loader_substituted=0 "
                "main_draws=4 active_tagged_draws=4 gl_error=0\n"
                "CHAIN_ACTIVE_GRID disabled_control=1 required_original=1 asset_enabled_first_stage_ordinary=1 "
                "busy_child_ordinary_published=1 denied_tail_exact_strong=1 available_tail_cooked=1 "
                "exact_owner_ready=1 actual_main_draw=1 immutable_streams=2 refnums=17 selected_refnums=16 "
                "mixed_occluder_original=1 picking_partitions=3 actual_pick_hits=1 picked_markers=1 "
                "vertex_partitions=2 whole_reference_markers=1 actual_merged_contributor=1 "
                "main_draws=4 active_tagged_draws=4 rgb=98 source_population_substituted=1 "
                "source_loader_substituted=0 gl_error=0\n"
                "CHAIN_ACTIVE_EPOCH source_changed=1 prepared_stale_deferred=1 old_guard_original=1 required_original_rebuilt=1 "
                "refnums_preserved=1 old_guard_not_revived=1 original_draws=2 rgb=98 gl_error=0\n"
                + AssetReleaseEvidence.asset_compile_receipt() + AssetReleaseEvidence.live_owner_adoption_receipt() +
                "CHAIN_ACTIVE_PREFIX_SETTLED after_scope=1 real_cache_maintenance=1 maintenance_passes=2 "
                "real_compiled_merge=1 compiled_after=0 "
                "asset_sets=0 pending_asset_sets=0 ordinary_roots=0 future=0 renderer_frames=0\n"
                "CHAIN_PASS source_population_substituted=1 source_loader_substituted=0 cook_substituted=0 "
                "readiness_substituted=0 cpu_limit=268435456\n")

    @staticmethod
    def live_owner_adoption_receipt():
        return ("CHAIN_LIVE_OWNER_ADOPTION real_world=1 rendered_weak_before_publication=1 fixed_camera_grid=1 "
                "publication_pending=1 pending_same_revision=1 ready_same_revision=1 exact_ready_owner=1 "
                "cache_only_no_demand=1 cache_only_no_ordinary_build=1 cache_only_no_source_or_cook=1 "
                "missing_unchanged=1 control_unchanged=1 disabled_unchanged=1 different_refnums_refused=1 stale_refused=1 "
                "immutable_old_pat=1 terrain_identity_order=1 transform_preserved=1 stable_adopted_pat=1 "
                "unrelated_publication_unchanged=1 actual_world_picking_original=1 canonical_pick_hits=2 pick_hits=2 "
                "main_draws=8 active_tagged_draws=8 rgb=98 terrain_population_substituted=1 source_population_substituted=1 "
                "source_loader_substituted=0 cook_substituted=0 callback_substituted=0 gl_error=0\n")

    @staticmethod
    def asset_compile_receipt():
        return ("CHAIN_ASSET_EXACT_CLOSURE real_openmw_ico=1 ico_mode=0 registered_context_held=1 "
                "pcp_inspection_context_rebound=1 ambient_program_handle_valid=1 unrelated_program_handle_valid=1 "
                "renderer_startup_empty_scene=1 renderer_startup_compile_consumed=1 renderer_compile_stayed_clear=1 unrelated_unlinked_after_original=1 "
                "ordinary_cache_only_pruned=1 pending_resource_set_survived=1 pending_host_accounted=1 exact_cached_owner=1 "
                "stock_imports_nonempty=1 distortion_zero=1 ambient_variant_different=1 exact_linked_variants=2 "
                "unrelated_unlinked_original=1 original_before_ico=2 global_exact_variants=0 main_draws=4 active_tagged_draws=4 "
                "state_input_substitution=1 source_loader_substituted=0 cook_substituted=0 callback_substituted=0 gl_error=0\n"
                "CHAIN_ASSET_COLD_VARIANT cold_owned_variant=1 original_draw_before_compile=0 initially_unlinked=1 "
                "pcp_inspection_context_rebound=1 cold_program_handle_valid=1 "
                "ordinary_compile_completed=1 ordinary_callback_delivered=1 observer_probe_cpu_only=1 observer_probe_after_compile_maps=1 "
                "ordinary_queue_observations=2 observer_pcp_before=0 observer_pcp_after=0 "
                "uninstalled_before_ready=1 real_openmw_ico=1 exact_linked_variants=2 distortion_zero=1 force_opaque_zero=1 "
                "global_exact_variants=0 main_draws=6 active_tagged_draws=6 rgb=99 state_input_substitution=1 "
                "source_loader_substituted=0 cook_substituted=0 callback_substituted=0 gl_error=0\n"
                "CHAIN_ASSET_COMPILE_RETIRE root_retired=1 retained_inner_cancelled=1 retained_quality_small=1 producer_expired=1 "
                "queued_leaves_charged=1 cancelled_sets_pruned=1 host_before_prune=1000 host_after_prune=600 "
                "experiment_before=100 experiment_queued=500 experiment_after_prune=100 worker_future=0 "
                "source_loader_substituted=0 cook_substituted=0 callback_substituted=0\n"
                "CHAIN_ASSET_MISSING_CONTEXT actual_empty_ico=1 actual_cook_attempt=1 original_topology_preserved=1 "
                "canonical_refnums_preserved=1 retry_retained=1 readiness_false=1 no_context_invented=1 ico_input_substituted=1 "
                "source_loader_substituted=0 cook_substituted=0 callback_substituted=0\n"
                "CHAIN_ASSET_CONTEXT_REFUSAL registered_context=1 current_context_missing=1 actual_program_leaf=1 "
                "producer_cancelled=1 readiness_false=1 dispatch_input_substituted=1 callback_substituted=0 gl_error=0\n"
                "CHAIN_ASSET_RELINK_REFUSAL original_applied_first=1 imported_applied_pcp=1 actual_program_leaf=1 "
                "unsafe_relink_refused=1 producer_cancelled=1 readiness_false=1 original_apply_repairs=1 "
                "state_cache_invalidated=1 dispatch_input_substituted=1 callback_substituted=0 gl_error=0\n")

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
        for mask in (1, 2):
            terminal = ("CHAIN_PASS source_population_substituted=1 source_loader_substituted=0 "
                        "cook_substituted=0 readiness_substituted=0 cpu_limit=268435456 "
                        f"common_budget_only=1 asset_mask={mask} child_budget_enabled=0 "
                        "common_retained_growth=8192 owned_callbacks=2 owned_ready=1 actual_derived_draws=4 "
                        "experiment_retained=0 experiment_future=0 experiment_jobs=0\n")
            gate.chain_common_owned_compile(terminal, mask)
            for field in terminal.split()[1:]:
                key, value = field.split("=", 1)
                replacements = ("", f"{key}=invalid", f"{field} {field}")
                if value in ("0", "1"):
                    replacements += (f"{key}={1-int(value)}",)
                for replacement in replacements:
                    with self.subTest(mask=mask, field=key, replacement=replacement), self.assertRaises(gate.QualificationError):
                        gate.chain_common_owned_compile(terminal.replace(" " + field, " " + replacement, 1), mask)
            for before, after in (("common_retained_growth=8192", "common_retained_growth=0"),
                                  ("owned_callbacks=2", "owned_callbacks=0"),
                                  ("owned_ready=1", "owned_ready=3"),
                                  ("actual_derived_draws=4", "actual_derived_draws=0"),
                                  (f"asset_mask={mask}", f"asset_mask={3-mask}")):
                with self.subTest(mask=mask, replacement=after), self.assertRaises(gate.QualificationError):
                    gate.chain_common_owned_compile(terminal.replace(before, after), mask)
            for bad in ("", terminal + terminal):
                with self.subTest(mask=mask, terminal=bad), self.assertRaises(gate.QualificationError):
                    gate.chain_common_owned_compile(bad, mask)

    def test_admission_wait_requires_renewed_actual_cook(self):
        expected = {"observations": 3, "construction_attempts_before_resume": 0,
                    "ordinary_preserved": 1, "exact_cached_denied_owner": 1,
                    "weak_graph_unchanged": 1, "denied_topology_unchanged": 1,
                    "renewed_actual_cook": 1, "generation_reset": 0}
        valid = "CHAIN_RETRY_ADMISSION_WAIT " + " ".join(f"{key}={value}" for key, value in expected.items()) + "\n"
        gate.chain_retry_admission_wait(valid)
        for bad in ("CHAIN_RETRY_EXHAUSTED actual_attempts=3 ordinary_preserved=1\n",
                    valid.replace("renewed_actual_cook=1", "renewed_actual_cook=0"),
                    valid.replace("generation_reset=0", "generation_reset=1"),
                    valid.replace("construction_attempts_before_resume=0", "construction_attempts_before_resume=3")):
            with self.subTest(receipt=bad), self.assertRaises(gate.QualificationError):
                gate.chain_retry_admission_wait(bad)
        for key in ("exact_cached_denied_owner", "weak_graph_unchanged", "denied_topology_unchanged"):
            for bad in (valid.replace(key + "=1", key + "=0"), valid.replace(" " + key + "=1", "")):
                with self.subTest(key=key, receipt=bad), self.assertRaises(gate.QualificationError):
                    gate.chain_retry_admission_wait(bad)

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

    def test_active_grid_requires_actual_origin_and_ordinary_interactions(self):
        valid = self.active_grid_receipt()
        proof = gate.l1_active_grid(valid)
        self.assertEqual(proof["actual_active_owner"]["active_tagged_draws"], "4")
        self.assertEqual(proof["strict_partial_tail"]["main_draws"], "4")
        self.assertEqual(len(proof["asset_compile_closure"]), 6)
        self.assertEqual(proof["asset_compile_closure"]["cold_variant"]["original_draw_before_compile"], "0")
        self.assertEqual(proof["retired_prefix_settled"]["pending_asset_sets"], "0")
        self.assertEqual(proof["live_owner_adoption"]["main_draws"], "8")
        self.assertEqual(proof["live_owner_adoption"]["canonical_pick_hits"], "2")
        for before, after in (("disabled_control=1", "disabled_control=0"),
                              ("required_original=1", "required_original=0"),
                              ("asset_enabled_first_stage_ordinary=1", "asset_enabled_first_stage_ordinary=0"),
                              ("busy_child_ordinary_published=1", "busy_child_ordinary_published=0"),
                              ("denied_tail_exact_strong=1", "denied_tail_exact_strong=0"),
                              ("available_tail_cooked=1", "available_tail_cooked=0"),
                              ("exact_owner_ready=1", "exact_owner_ready=0"),
                              ("immutable_streams=2", "immutable_streams=0"),
                              ("refnums=17", "refnums=0"),
                              ("refnums=17", "refnums=3"),
                              ("selected_refnums=16", "selected_refnums=17"),
                              ("selected_refnums=16", "selected_refnums=2"),
                              ("candidate_instances=16", "candidate_instances=2"),
                              ("ordinary_instances=1", "ordinary_instances=0"),
                              ("total_instances=17", "total_instances=3"),
                              ("captured_merge_settings=1", "captured_merge_settings=0"),
                              ("vertex_partitions=2", "vertex_partitions=0"),
                              ("whole_reference_markers=1", "whole_reference_markers=0"),
                              ("actual_merged_contributor=1", "actual_merged_contributor=0"),
                              ("picking_partitions=3", "picking_partitions=0"),
                              ("actual_pick_hits=1", "actual_pick_hits=0"),
                              ("picked_markers=1", "picked_markers=0"),
                              ("main_draws=4", "main_draws=0"),
                              ("active_tagged_draws=4", "active_tagged_draws=0"),
                              ("active_tagged_draws=4", "active_tagged_draws=3"),
                              ("rgb=98", "rgb=16"),
                              ("refnums_preserved=1", "refnums_preserved=0"),
                              ("prepared_stale_deferred=1", "prepared_stale_deferred=0"),
                              ("old_guard_not_revived=1", "old_guard_not_revived=0"),
                              ("original_draws=2", "original_draws=0"),
                              ("readiness_substituted=0", "readiness_substituted=1"),
                              ("source_loader_substituted=0", "source_loader_substituted=1")):
            with self.subTest(after=after), self.assertRaises(gate.QualificationError):
                gate.l1_active_grid(valid.replace(before, after))
        partial_line = next(line for line in valid.splitlines() if line.startswith("CHAIN_ACTIVE_PARTIAL_TAIL "))
        for key in ("missing_skipped", "missing_unpublished", "weak_exact", "weak_unchanged",
                    "skipped_no_ordinary_build", "skipped_no_asset_attempt", "insufficient_normal_exact",
                    "wrong_spatial_exact", "busy_exact",
                    "available_cooked", "incomplete_retry_retained", "exact_owner_ready"):
            for replacement in ("", f" {key}=0"):
                bad_line = partial_line.replace(f" {key}=1", replacement)
                with self.subTest(partial_field=key, replacement=replacement), self.assertRaises(gate.QualificationError):
                    gate.l1_active_grid(valid.replace(partial_line, bad_line))
        for before, after in (("main_draws=4", ""),
                              ("main_draws=4", "main_draws=0"),
                              ("main_draws=4", "main_draws=3"),
                              ("active_tagged_draws=4", ""),
                              ("active_tagged_draws=4", "active_tagged_draws=0"),
                              ("active_tagged_draws=4", "active_tagged_draws=3"),
                              ("source_loader_substituted=0", ""),
                              ("source_loader_substituted=0", "source_loader_substituted=1"),
                              ("gl_error=0", ""),
                              ("gl_error=0", "gl_error=1")):
            with self.subTest(partial_field=before, replacement=after), self.assertRaises(gate.QualificationError):
                gate.l1_active_grid(valid.replace(partial_line, partial_line.replace(before, after)))
        # Every field in the actual compile and late-adoption receipts is mandatory and
        # independently validated. A neighboring correct record cannot hide a
        # missing, malformed, duplicated or reversed proof flag.
        for line in (self.asset_compile_receipt() + self.live_owner_adoption_receipt()).splitlines():
            marker, *fields = line.split()
            for field in fields:
                key, value = field.split("=", 1)
                changes = [("missing", ""), ("invalid", f"{key}=invalid"),
                           ("duplicate", f"{field} {field}")]
                if value in ("0", "1"):
                    changes.append(("reversed", f"{key}={1-int(value)}"))
                for kind, replacement in changes:
                    bad_line = line.replace(" " + field, " " + replacement, 1)
                    with self.subTest(marker=marker, field=key, invalid=kind), self.assertRaises(gate.QualificationError):
                        gate.l1_active_grid(valid.replace(line, bad_line))
        prefix_line = next(line for line in valid.splitlines() if line.startswith("CHAIN_ACTIVE_PREFIX_SETTLED "))
        for field in prefix_line.split()[1:]:
            key, value = field.split("=", 1)
            for replacement in ("", f"{key}=invalid", f"{field} {field}", f"{key}={1-int(value)}"):
                bad_line = prefix_line.replace(" " + field, " " + replacement, 1)
                with self.subTest(prefix_field=key, replacement=replacement), self.assertRaises(gate.QualificationError):
                    gate.l1_active_grid(valid.replace(prefix_line, bad_line))
        for marker, before, after in (
                ("CHAIN_ASSET_EXACT_CLOSURE", "main_draws=4", "main_draws=3"),
                ("CHAIN_ASSET_EXACT_CLOSURE", "active_tagged_draws=4", "active_tagged_draws=3"),
                ("CHAIN_ASSET_EXACT_CLOSURE", "original_before_ico=2", "original_before_ico=0"),
                ("CHAIN_ASSET_COLD_VARIANT", "main_draws=6", "main_draws=0"),
                ("CHAIN_ASSET_COLD_VARIANT", "ordinary_queue_observations=2", "ordinary_queue_observations=0"),
                ("CHAIN_ASSET_COLD_VARIANT", "active_tagged_draws=6", "active_tagged_draws=5"),
                ("CHAIN_ASSET_COLD_VARIANT", "rgb=99", "rgb=16"),
                ("CHAIN_ASSET_COMPILE_RETIRE", "host_after_prune=600", "host_after_prune=1000"),
                ("CHAIN_ASSET_COMPILE_RETIRE", "experiment_queued=500", "experiment_queued=100"),
                ("CHAIN_ASSET_COMPILE_RETIRE", "experiment_after_prune=100", "experiment_after_prune=500")):
            line = next(row for row in valid.splitlines() if row.startswith(marker + " "))
            with self.subTest(marker=marker, change=after), self.assertRaises(gate.QualificationError):
                gate.l1_active_grid(valid.replace(line, line.replace(before, after)))
        adoption_line = self.live_owner_adoption_receipt().strip()
        for before, after in (("main_draws=8", "main_draws=0"),
                              ("active_tagged_draws=8", "active_tagged_draws=7"),
                              ("pick_hits=2", "pick_hits=0"),
                              ("canonical_pick_hits=2", "canonical_pick_hits=1"),
                              ("rgb=98", "rgb=16")):
            with self.subTest(adoption_change=after), self.assertRaises(gate.QualificationError):
                gate.l1_active_grid(valid.replace(adoption_line, adoption_line.replace(" " + before, " " + after, 1)))
        for key in ("main_draws", "active_tagged_draws", "pick_hits", "canonical_pick_hits", "rgb"):
            value = gate.live_owner_adoption(valid)[key]
            for invalid in ("-1", "18446744073709551616"):
                bad_line = adoption_line.replace(f" {key}={value}", f" {key}={invalid}", 1)
                with self.subTest(adoption_field=key, invalid=invalid), self.assertRaises(gate.QualificationError):
                    gate.l1_active_grid(valid.replace(adoption_line, bad_line))
        for marker in ("CHAIN_ASSET_EXACT_CLOSURE", "CHAIN_ASSET_COLD_VARIANT"):
            line = next(row for row in valid.splitlines() if row.startswith(marker + " "))
            for value in ("0", "513", "-1", "18446744073709551616"):
                with self.subTest(marker=marker, variants=value), self.assertRaises(gate.QualificationError):
                    gate.l1_active_grid(valid.replace(line, line.replace("exact_linked_variants=2", "exact_linked_variants=" + value)))

    def test_active_grid_missing_ambiguous_and_invalid_receipts_fail_closed(self):
        valid = self.active_grid_receipt()
        for marker in ("CHAIN_ACTIVE_POPULATION", "CHAIN_ACTIVE_GRID", "CHAIN_ACTIVE_PARTIAL_TAIL", "CHAIN_ACTIVE_EPOCH", "CHAIN_ACTIVE_PREFIX_SETTLED", "CHAIN_LIVE_OWNER_ADOPTION", "CHAIN_PASS",
                       "CHAIN_ASSET_EXACT_CLOSURE", "CHAIN_ASSET_COLD_VARIANT", "CHAIN_ASSET_COMPILE_RETIRE",
                       "CHAIN_ASSET_MISSING_CONTEXT", "CHAIN_ASSET_CONTEXT_REFUSAL", "CHAIN_ASSET_RELINK_REFUSAL"):
            missing = "\n".join(line for line in valid.splitlines() if not line.startswith(marker + " ")) + "\n"
            duplicate = valid + next(line for line in valid.splitlines() if line.startswith(marker + " ")) + "\n"
            for bad in (missing, duplicate):
                with self.subTest(marker=marker, bad=bad), self.assertRaises(gate.QualificationError):
                    gate.l1_active_grid(bad)
        for bad in ("SKIP: no actual GL context\n" + valid,
                    valid.replace("main_draws=4", "main_draws=4 main_draws=4"),
                    valid.replace("main_draws=4", "main_draws=-4"),
                    valid.replace("active_tagged_draws=4", "active_tagged_draws=18446744073709551616"),
                    valid.replace("immutable_streams=2", "immutable_streams=unknown"),
                    valid.replace(" picked_markers=1", ""),
                    valid.replace(" actual_merged_contributor=1", ""),
                    valid.replace(" prepared_stale_deferred=1", ""),
                    valid.replace("cpu_limit=268435456", "cpu_limit=134217728"),
                    "CHAIN_FAILURE active_missing\n" + valid):
            with self.subTest(bad=bad), self.assertRaises(gate.QualificationError):
                gate.l1_active_grid(bad)

    def test_historical_distant_population_is_independent_of_active_population(self):
        distant = ("CHAIN_MIXED_INPUT candidate_instances=2 spatial_buckets=2 ordinary_instances=1 "
                   "actual_stat_sources=1 material_substitution=0\n")
        expected = {"candidate_instances": 2, "spatial_buckets": 2, "ordinary_instances": 1,
                    "actual_stat_sources": 1, "material_substitution": 0}
        gate.record(distant, "CHAIN_MIXED_INPUT", expected)
        active = self.active_grid_receipt()
        gate.l1_active_grid(distant + active)
        with self.assertRaises(gate.QualificationError):
            gate.record(distant.replace("candidate_instances=2", "candidate_instances=16"),
                        "CHAIN_MIXED_INPUT", expected)


if __name__ == "__main__":
    unittest.main()
