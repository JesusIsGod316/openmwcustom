#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
npc = (root / "apps/openmw/mwrender/npcanimation.cpp").read_text()
animation = (root / "apps/openmw/mwrender/animation.cpp").read_text()
terrain_h = (root / "components/terrain/chunkmanager.hpp").read_text()
terrain = (root / "components/terrain/chunkmanager.cpp").read_text()
prep = (root / "components/sceneutil/prepjobservice.hpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
launcher = (root / "tools/optimizedmw/gl-p7/OptimizedMW_GL-P7_Test.ps1").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "p6r_build_repair": "#include <osgUtil/Statistics>" in engine
        and "#include <osgUtil/StatsVisitor>" not in engine,
    "default_off": "optimizedmw parallel actor binding = false" in defaults
        and "optimizedmw parallel terrain cpu prep = false" in defaults,
    "settings_declared": "mOptimizedMWParallelActorBinding" in cells
        and "mOptimizedMWParallelTerrainCpuPrep" in cells,
    "accepted_actor_batch_reactivated": "mOptimizedMWParallelActorBinding" in npc
        and "beginAnimSourceBatch()" in npc and "endAnimSourceBatch()" in npc
        and "mOptimizedMWParallelActorBinding" in animation
        and "FrameCriticalJobGroup::instance().parallelFor" in animation
        and "Main-thread deterministic publication" in animation,
    "terrain_two_way_prep": "PreparedPassData" in terrain_h
        and "preparePassData" in terrain and "mStorage->getBlendmaps" in terrain
        and "fillVertices" in terrain and "PrepJobService::instance().runPair" in terrain
        and "Lane::Critical" in terrain and "Lane::Background" in terrain
        and "if (!paired)" in terrain,
    "queue_less_prep_service": "queue-less two-way CPU preparation" in prep
        and "busyFallbacks" in prep and "mAwaitingAck" in prep
        and "OPENMW_P7_PREP_STATS_FILE" in prep,
    "benchmark_matrix": all(token in launcher for token in (
        "P6-QUARANTINE-CONTROL", "P7-ACTOR-BATCH", "P7-TERRAIN-CPU",
        "P7-ACTOR-TERRAIN", "P7-ACTOR-TERRAIN-RESOURCES", "P7-FULL-STUTTER")),
    "benchmark_telemetry_only": "OPENMW_P6_TRAVERSAL_FILE" in launcher
        and "OPENMW_V325_JOBGROUP_STATS_FILE" in launcher
        and "OPENMW_P7_PREP_STATS_FILE" in launcher
        and "preload num threads' '1'" in launcher,
    "launcher_packaged": "START-OptimizedMW-GL-P7-Test.bat" in cmake
        and "OptimizedMW_GL-P7_Test.ps1" in cmake,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P7 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P7 source contract passed")
