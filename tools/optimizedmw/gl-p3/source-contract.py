#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
optimizer_h = (root / "components/sceneutil/optimizer.hpp").read_text()
optimizer_cpp = (root / "components/sceneutil/optimizer.cpp").read_text()
objectpaging_h = (root / "apps/openmw/mwrender/objectpaging.hpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
quadtree_h = (root / "components/terrain/quadtreeworld.hpp").read_text()
quadtree_cpp = (root / "components/terrain/quadtreeworld.cpp").read_text()
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
speculative = (root / "components/resource/speculativebudget.hpp").read_text()
paging = (root / "components/sceneutil/pagingwork.hpp").read_text()
two_way = (root / "components/sceneutil/boundedtwowaywork.hpp").read_text()

p3_submission = objectpaging.index("const bool p3SubmissionCompaction")
v38_before_p3 = objectpaging.rfind("const int v38BatchingMode", 0, p3_submission)
p2_before_p3 = objectpaging.rfind("const bool p2RequiredReadiness", 0, p3_submission)

launcher = (root / "tools/optimizedmw/gl-p3/OptimizedMW_GL-P3_Test.ps1").read_text()
cmake_root = (root / "CMakeLists.txt").read_text()

checks = {
    "p3a_retained_but_parked": "optimizedmw submission compaction" in cells
        and "optimizedmw submission compaction = false" in defaults
        and "optimizedmw submission compaction' $P3A" in launcher
        and "$P3A='false'" in launcher,
    "focused_launcher": all(token in launcher for token in (
        "CONTROL", "P3B-1HELPER", "P3B-2HELPER", "P3C-OPTIONAL-PREMERGE",
        "P3D-DISPLAYLIST", "P3E-IGNORED-COLOR-STRIP", "P3F-SHADOW-BATCHING",
        "P3-CORE-B2-D-F", "P3-ALL-REPAIRED",
        "optimizedmw render handoff attribution", "settings-effective-test.cfg",
        "OPENMW_V36_BATCHING_FILE"))
        and "START-OptimizedMW-GL-P3-Test.bat" in cmake_root
        and "OptimizedMW_GL-P3_Test.ps1" in cmake_root,
    "switchable_optimizer": "setMergeCompatibleIndexTypes" in optimizer_h
        and "setPreferDisplayListsForMergedGeometry" in optimizer_h
        and "setNormalizeIgnoredVertexColors" in optimizer_h,
    "mixed_width_merge_retained": "mergePromotedDrawElements" in optimizer_cpp
        and "p3_index_merges=" in objectpaging,
    "production_declaration_order": v38_before_p3 >= 0 and p2_before_p3 >= 0,
    "optional_phase_contract": "static bool optionalOptimization()" in paging,
    "distant_optional_route": "supportsDistantStrongPagingUpgrade" in quadtree_h
        and "supportsDistantStrongPagingUpgrade" in quadtree_cpp
        and "supportsDistantStrongPagingUpgrade() const override" in objectpaging_h
        and "p3OptionalDistantMask" in objectpaging
        and "mP3OptionalMask" in objectpaging_h
        and "SceneUtil::PagingWorkScope::optionalOptimization()" in objectpaging,
    "p3c_optional_only": "const bool p3SemanticPremerge" in objectpaging
        and "&& p3OptionalDistant && v38BatchingMode >= 2" in objectpaging,
    "p3d_optional_only": "const bool p3DistantDisplayLists" in objectpaging
        and "&& p3OptionalDistant && !static_cast<bool>(Settings::stereo().mMultiview)" in objectpaging,
    "p3e_optional_only": "const bool p3NormalizedStaticPackets" in objectpaging
        and "&& p3OptionalDistant && v38BatchingMode >= 2" in objectpaging,
    "p3e_semantic_strip": "VertexColorModes::None" in optimizer_cpp
        and "geometry->setColorArray(nullptr)" in optimizer_cpp
        and "p3_normalized_colors=" in objectpaging,
    "p3f_optional_only": "const bool p3ShadowStaticBatching" in objectpaging
        and "&& p3OptionalDistant;" in objectpaging,
    "p3f_diagnostics": all(token in objectpaging for token in (
        "p3_shadow_candidates=", "p3_shadow_eligible=", "p3_shadow_rejected_state=",
        "p3_shadow_rejected_geometry=", "p3_shadow_batches=", "p3_shadow_indices=")),
    "thread_switch": "optimizedmw parallel template prefetch" in cells
        and "optimizedmw parallel template prefetch = false" in defaults
        and "optimizedmw parallel template prefetch workers" in cells
        and "optimizedmw parallel template prefetch workers = 2" in defaults,
    "bounded_helpers": "class BoundedTwoWayWork" in two_way
        and "std::try_to_lock" in two_way
        and "runWorkers" in two_way
        and "std::clamp(requestedHelpers, std::size_t{ 1 }, std::size_t{ 2 })" in two_way
        and "std::vector<std::thread> mWorkers" in two_way
        and "no FIFO/background queue" in two_way,
    "speculative_context": "static Context capture()" in speculative
        and "creditRetainedEstimate" in speculative,
    "paging_context": "static Context capture()" in paging,
    "p3b_distant_only": "&& !activeGrid && compile;" in objectpaging
        and "p3TemplatePrefetchRequired" in objectpaging,
    "p3b_dynamic_balance": "nextModel.fetch_add" in objectpaging
        and "BoundedTwoWayWork::runWorkers" in objectpaging
        and "mOptimizedMWParallelTemplatePrefetchWorkers" in objectpaging,
    "p3b_mechanical_counters": all(token in objectpaging for token in (
        "p3_prefetch_parallel=", "p3_prefetch_helpers=", "p3_prefetch_reuse_hits=",
        "p3PrefetchedTemplates.find(model)")),
    "render_handoff_diagnostic": "optimizedmw render handoff attribution" in cells
        and "optimizedmw render handoff attribution = false" in defaults,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("P3 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P3 integration-repair source contract passed")
