#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
compile_h = (root / "components/resource/openmwcompileoperation.hpp").read_text()
compile_cpp = (root / "components/resource/openmwcompileoperation.cpp").read_text()
policy = (root / "components/resource/p4compilepolicy.hpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
terrain_chunk = (root / "components/terrain/chunkmanager.cpp").read_text()
terrain_drawable_h = (root / "components/terrain/terraindrawable.hpp").read_text()
terrain_drawable_cpp = (root / "components/terrain/terraindrawable.cpp").read_text()
scene_manager = (root / "components/resource/scenemanager.cpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
diagnostics = (root / "components/debug/v3diagnostics.hpp").read_text()
cmake = (root / "components/CMakeLists.txt").read_text()
root_cmake = (root / "CMakeLists.txt").read_text()
launcher = (root / "tools/optimizedmw/gl-p4/OptimizedMW_GL-P4_Test.ps1").read_text()

checks = {
    "mode_default_off": "optimizedmw compile scheduler mode = 0" in defaults
        and "mOptimizedMWCompileSchedulerMode" in cells,
    "scheduler_limits": all(token in cells for token in (
        "mOptimizedMWCompileSchedulerMaxBudgetMs",
        "mOptimizedMWCompileSchedulerCreditCapMs",
        "mOptimizedMWCompileSchedulerHeadroomRatio",
        "mOptimizedMWCompileSchedulerHandoffThresholdMs",
        "mOptimizedMWCompileSchedulerMaxQueueAgeFrames",
        "mOptimizedMWCompileSchedulerDeleteBudgetMs",
        "mOptimizedMWCompileSchedulerMaxObjectsPerFrame",
        "mOptimizedMWHeavyCompileLaneMode",
        "mOptimizedMWHeavyCompileThresholdMs",
        "mOptimizedMWHeavyCompileMinSmoothFrames",
        "mOptimizedMWHeavyCompileMinHeadroomMs",
        "mOptimizedMWTerrainDrawablePriorMs",
        "mOptimizedMWTerrainPhasedCompile")),
    "production_build": "openmwcompileoperation" in cmake,
    "control_exact_path": "new osgUtil::IncrementalCompileOperation" in rendering
        and "new Resource::OpenMWIncrementalCompileOperation" in rendering,
    "handoff_feedback": "publishRenderingTraversalMs" in engine
        and "mOptimizedMWCompileSchedulerMode) >= 2" in engine,
    "cost_aware": "predictedMs" in compile_cpp
        and "estimatedTimeForCompile" in compile_cpp
        and "observe(selected.mSet, selected.mKind, actualMs)" in compile_cpp
        and "mRiskMs" in compile_h
        and "costIndex" in compile_cpp,
    "cheap_drain_repair": "age never converts a cheap fitting operation" in compile_cpp
        and "if (fits)" in compile_cpp
        and "if (forced || remainingBudgetMs <= 0.0)" in compile_cpp
        and "mMaxObjectsPerFrame = 12" in compile_h,
    "heavy_lane": "heavy_smooth_headroom" in compile_cpp
        and "mHeavyMinSmoothFrames" in compile_h
        and "mTerrainDrawablePriorMs" in compile_h
        and "mSmoothFrames = 0" in compile_cpp,
    "terrain_phased_compile": "mOptimizedMWTerrainPhasedCompile" in cells
        and "phaseTerrainCompileSet" in terrain_chunk
        and "TerrainStateAttributeCompileOp" in terrain_chunk
        and "TerrainGeometryCompileOp" in terrain_chunk
        and "p5_terrain_pass_attribute" in terrain_chunk
        and "p5_terrain_geometry_vbo" in terrain_chunk
        and "compileGeometryGLObjects" in terrain_drawable_h
        and "osg::Geometry::compileGLObjects(renderInfo)" in terrain_drawable_cpp,
    "queue_age_guard": "forced_by_queue_age" in compile_cpp
        and "mConfig.mMaxQueueAgeFrames" in compile_cpp,
    "class_priority": "V321CompileClass::ObjectPaging" in compile_cpp
        and "V321CompileClass::Terrain" in compile_cpp
        and "new Resource::V321ClassifiedCompileSet" in objectpaging
        and "Resource::V321CompileClass::ObjectPaging" in objectpaging
        and "Resource::V321CompileClass::Terrain" in terrain_chunk
        and "compileClass != V321CompileClass::Unknown" in scene_manager,
    "separate_delete_budget": "separate_delete_budget" in compile_cpp
        and "mConfig.mDeleteBudgetMs" in compile_cpp,
    "nonblocking_diagnostics": "OPENMW_P4_COMPILE_FILE" in diagnostics
        and "DiagnosticWriterHub" in diagnostics,
    "policy_handoff_suppression": "mSuppressedByHandoff" in policy
        and "state.mCreditMs *= 0.25" in policy,
    "launcher_matrix": all(token in launcher for token in (
        "CONTROL-B1", "CONTROL-B1-C", "P4R-B1", "P4R-B1-C",
        "P4R-B1-C-HEAVY", "P4R-B1-C-TERRAIN", "P4R-B1-C-HEAVY-TERRAIN",
        "OPENMW_P4_COMPILE_FILE", "optimizedmw compile scheduler mode")),
    "launcher_packaged": "START-OptimizedMW-GL-P4-Test.bat" in root_cmake
        and "OptimizedMW_GL-P4_Test.ps1" in root_cmake,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P4 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P4 source contract passed")
