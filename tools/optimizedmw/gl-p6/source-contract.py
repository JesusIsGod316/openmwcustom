#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
classified = (root / "components/resource/v321classifiedcompileset.hpp").read_text()
compile_h = (root / "components/resource/openmwcompileoperation.hpp").read_text()
compile_cpp = (root / "components/resource/openmwcompileoperation.cpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
terrain = (root / "components/terrain/chunkmanager.cpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
hitch = (root / "components/debug/v3hitchtelemetry.hpp").read_text()
diag = (root / "components/debug/v3diagnostics.hpp").read_text()
launcher = (root / "tools/optimizedmw/gl-p6/OptimizedMW_GL-P6_Test.ps1").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "default_off": all(token in defaults for token in (
        "optimizedmw residency scheduler mode = 0",
        "optimizedmw terrain immutable vertex reuse = false",
        "optimizedmw texture pbo staging = false")),
    "settings_declared": all(token in cells for token in (
        "mOptimizedMWResidencySchedulerMode",
        "mOptimizedMWTerrainImmutableVertexReuse",
        "mOptimizedMWTexturePboStaging")),
    "deadline_metadata": "V321CompileUrgency" in classified
        and "getV321CompileUrgency" in classified
        and "activeGrid ? Resource::V321CompileUrgency::NearFuture" in terrain
        and "activeGrid ? Resource::V321CompileUrgency::NearFuture" in objectpaging,
    "scheduler_quarantine": "quarantineHeavy" in compile_cpp
        and "quarantined_candidates=" in compile_cpp
        and "mResidencySchedulerMode" in compile_h
        and "mOptimizedMWResidencySchedulerMode" in rendering,
    "size_aware_cost": "resourceSizeBytes" in compile_cpp
        and "resourceSizeTier" in compile_cpp
        and "sCompileSizeTierCount = 4" in compile_h,
    "terrain_vertex_reuse": "mOptimizedMWTerrainImmutableVertexReuse" in terrain
        and "arrays and their VBO" in terrain,
    "texture_pbo": "_assignPBOToImages" in terrain
        and "mOptimizedMWTexturePboStaging" in terrain
        and "mOptimizedMWTexturePboStaging" in objectpaging,
    "benchmark_only_hitch": "Normal gameplay must never start" in hitch
        and "OPENMW_V3_HITCH_FILE" in launcher
        and "OPENMW_V3_FRAME_FILE" in launcher,
    "swap_attribution": "P6SwapTimingCallback" in engine
        and "p6RenderPhaseWriter().enabled()" in engine
        and "OPENMW_P6_RENDER_PHASE_FILE" in diag
        and "OPENMW_P6_RENDER_PHASE_FILE" in launcher,
    "launcher_matrix": all(token in launcher for token in (
        "P4R-STAGE2", "P6-QUARANTINE", "P6-STRICT-QUARANTINE",
        "P6-QUARANTINE-VERTEX-REUSE", "P6-QUARANTINE-PBO", "P6-FULL")),
    "launcher_packaged": "START-OptimizedMW-GL-P6-Test.bat" in cmake
        and "OptimizedMW_GL-P6_Test.ps1" in cmake,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P6 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P6 source contract passed")
