#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cell = (root / "apps/openmw/mwworld/cellpreloader.cpp").read_text()
paging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
paging_h = (root / "apps/openmw/mwrender/objectpaging.hpp").read_text()
terrain = (root / "components/terrain/quadtreeworld.cpp").read_text()
terrain_h = (root / "components/terrain/quadtreeworld.hpp").read_text()
budget = (root / "components/resource/speculativebudget.hpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
scope = (root / "components/sceneutil/pagingwork.hpp").read_text()
settings = (root / "components/settings/categories/cells.hpp").read_text()

checks = {
    "phase enum": "RequiredReadiness" in scope and "OptionalOptimization" in scope,
    "readiness setting": "mOpimizedMWPagingReadinessSplit" in settings,
    "safe quality gate": "mV313ChunkQualityMode) > 0" in cell and "mV311ActiveGridPrepareMode) > 0" in cell,
    "required reporter": "mRequiredReporter.wait(listener)" in cell,
    "no optional join in split wait": "Do not join the optional strong-upgrade tail" in cell,
    "readiness publication": "takeReadinessPublication()" in cell,
    "obsolete optional cancel": "mTerrainPreloadItem->readinessComplete()" in cell and "mTerrainPreloadItem->abort();" in cell,
    "object quality weakened only for readiness": "activeGrid && compile && !p2RequiredReadiness" in paging,
    "vertex reorder removed from readiness": "options &= ~(SceneUtil::Optimizer::VERTEX_POSTTRANSFORM | SceneUtil::Optimizer::VERTEX_PRETRANSFORM)" in paging,
    "GL compile remains enabled": "if (compile)" in paging and "mergeGroup->accept(stateToCompile)" in paging,
    "optional pass targets ObjectPaging directly": "preloadStrongUpgrade" in cell
        and "preloadStrongUpgrade" in terrain and "supportsStrongPagingUpgrade" in terrain_h,
    "ObjectPaging opts into strong upgrade": "supportsStrongPagingUpgrade() const override" in paging_h,
    "optional pass does not replay cached preload": "prepare(optimizationReporter)" not in cell,
    "weak private wrapper released": "entry.mRenderingNode = nullptr" in terrain,
    "P1 near-future priority exists": "SpeculativePriority::NearFuture" in cell
        and "reservedNearFutureJobs" in budget,
    "P1 reserves upgrade slot only with readiness split": "mOpimizedMWPagingReadinessSplit" in engine
        and "reservedNearFutureJobs" in engine,
}
failed=[name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("P2 readiness contract failed: " + ", ".join(failed))
print(f"{len(checks)} P2 readiness source contracts passed")
