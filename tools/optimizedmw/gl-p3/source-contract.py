#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
optimizer_h = (root / "components/sceneutil/optimizer.hpp").read_text()
optimizer_cpp = (root / "components/sceneutil/optimizer.cpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
speculative = (root / "components/resource/speculativebudget.hpp").read_text()
paging = (root / "components/sceneutil/pagingwork.hpp").read_text()
two_way = (root / "components/sceneutil/boundedtwowaywork.hpp").read_text()

checks = {
    "setting": "optimizedmw submission compaction" in cells and "optimizedmw submission compaction = false" in defaults,
    "switchable_optimizer": "setMergeCompatibleIndexTypes" in optimizer_h,
    "mixed_width_merge": "mergePromotedDrawElements" in optimizer_cpp,
    "strong_only_gate": "&& compile && !p2RequiredReadiness && v38BatchingMode >= 2" in objectpaging,
    "mechanical_counter": "p3_index_merges=" in objectpaging,
    "thread_switch": "optimizedmw parallel template prefetch" in cells
        and "optimizedmw parallel template prefetch = false" in defaults,
    "bounded_helper": "class BoundedTwoWayWork" in two_way and "std::try_to_lock" in two_way
        and "workerLoop" in two_way and "mWorker.joinable()" in two_way,
    "speculative_context": "static Context capture()" in speculative
        and "creditRetainedEstimate" in speculative,
    "paging_context": "static Context capture()" in paging,
    "distant_only_threading": "&& !activeGrid && compile;" in objectpaging
        and "p3TemplatePrefetchRequired" in objectpaging,
    "thread_mechanical_counter": "p3_prefetch_parallel=" in objectpaging,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("P3 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P3 source contract passed")
