#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
compile_h = (root / "components/resource/openmwcompileoperation.hpp").read_text()
compile_cpp = (root / "components/resource/openmwcompileoperation.cpp").read_text()
compile_ops = (root / "components/resource/p4compileops.hpp").read_text()
policy = (root / "components/resource/p4compilepolicy.hpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
terrain_chunk = (root / "components/terrain/chunkmanager.cpp").read_text()
terrain_compile = (root / "components/terrain/terraincompile.cpp").read_text()
scene_manager = (root / "components/resource/scenemanager.cpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
diagnostics = (root / "components/debug/v3diagnostics.hpp").read_text()
cmake = (root / "components/CMakeLists.txt").read_text()
p4_cmake = (root / "tools/optimizedmw/gl-p4/CMakeLists.txt").read_text()
root_cmake = (root / "CMakeLists.txt").read_text()
launcher = (root / "tools/optimizedmw/gl-p4/OptimizedMW_GL-P4_Test.ps1").read_text()

checks = {
    "mode_default_off": "optimizedmw compile scheduler mode = 0" in defaults
        and "mOptimizedMWCompileSchedulerMode" in cells,
    "new_bottleneck_defaults_off":
        "optimizedmw staged terrain compile = false" in defaults
        and "optimizedmw split terrain attribute vbos = false" in defaults
        and "mOptimizedMWStagedTerrainCompile" in cells
        and "mOptimizedMWSplitTerrainAttributeVbos" in cells,
    "scheduler_repair_limits":
        "optimizedmw compile scheduler max queue age frames = 60" in defaults
        and "optimizedmw compile scheduler max objects per frame = 12" in defaults
        and "optimizedmw compile scheduler minimum drain budget ms = 0.35" in defaults
        and "mOptimizedMWCompileSchedulerMinimumDrainBudgetMs" in cells,
    "production_build":
        "openmwcompileoperation" in cmake and "terraincompile" in cmake,
    "control_exact_path":
        "new osgUtil::IncrementalCompileOperation" in rendering
        and "new Resource::OpenMWIncrementalCompileOperation" in rendering,
    "handoff_feedback":
        "publishRenderingTraversalMs" in engine
        and "mOptimizedMWCompileSchedulerMode) >= 2" in engine,
    "class_kind_cost_model":
        "predictedCost" in compile_cpp
        and "mEstimateScale" in compile_h
        and "mHighWaterMs" in compile_h
        and "mCosts.at(compileClassIndex(set))" in compile_cpp
        and "mOsgEstimateMs" in compile_h,
    "scheduler_repairs_old_age_bug":
        "Candidate bestFit" in compile_cpp
        and "Candidate bestForced" in compile_cpp
        and "budgeted_aged_cheap" in compile_cpp
        and "if (forced || remainingBudgetMs <= 0.0)" in compile_cpp
        and "forced_by_queue_age_over_budget" in compile_cpp,
    "small_drain_floor":
        "mMinimumDrainBudgetMs" in compile_cpp
        and "queued.size() >= 1024" in compile_cpp
        and "queued.size() >= 256" in compile_cpp,
    "class_priority":
        "V321CompileClass::ObjectPaging" in compile_cpp
        and "V321CompileClass::Terrain" in compile_cpp
        and "new Resource::V321ClassifiedCompileSet" in objectpaging
        and "Resource::V321CompileClass::ObjectPaging" in objectpaging
        and "Resource::V321CompileClass::Terrain" in terrain_chunk
        and "compileClass != V321CompileClass::Unknown" in scene_manager,
    "separate_delete_budget":
        "separate_delete_budget" in compile_cpp
        and "mConfig.mDeleteBudgetMs" in compile_cpp,
    "staged_terrain_compile":
        "buildStagedTerrainCompileMap" in terrain_compile
        and "P4CompileBufferOp" in terrain_compile
        and "P4CompileStateSetOp" in terrain_compile
        and "P4CompileGeometryFinalizeOp" in terrain_compile
        and "getArrayList" in terrain_compile
        and "getDrawElementsList" in terrain_compile
        and "mOptimizedMWStagedTerrainCompile" in terrain_chunk,
    "split_attribute_vbos":
        "mOptimizedMWSplitTerrainAttributeVbos" in terrain_chunk
        and terrain_chunk.count("positions->setVertexBufferObject(new osg::VertexBufferObject)") >= 2
        and terrain_chunk.count("normals->setVertexBufferObject(new osg::VertexBufferObject)") >= 2
        and terrain_chunk.count("colors->setVertexBufferObject(new osg::VertexBufferObject)") >= 2,
    "buffer_op_real_compile":
        "getOrCreateGLBufferObject" in compile_ops
        and "glBuffer->compileBuffer()" in compile_ops
        and "mGeometry->osg::Geometry::compileGLObjects(info)" in compile_ops,
    "expanded_nonblocking_diagnostics":
        "OPENMW_P4_COMPILE_FILE" in diagnostics
        and "osg_estimate_ms" in diagnostics
        and "estimate_scale" in diagnostics
        and "fits_budget" in diagnostics
        and "DiagnosticWriterHub" in diagnostics,
    "policy_handoff_suppression":
        "mSuppressedByHandoff" in policy
        and "state.mCreditMs *= 0.25" in policy,
    "qc_compiles_new_path":
        "components/terrain/terraincompile.cpp" in p4_cmake
        and "components/resource/openmwcompileoperation.cpp" in p4_cmake,
    "launcher_matrix": all(token in launcher for token in (
        "CONTROL-B1-C", "P4R-B1", "P4R-B1-C", "P4R-B1-C-STAGED",
        "P4R-B1-C-STAGED-SPLITVBO", "OPENMW_P4_COMPILE_FILE",
        "optimizedmw staged terrain compile", "optimizedmw split terrain attribute vbos")),
    "launcher_packaged":
        "START-OptimizedMW-GL-P4-Test.bat" in root_cmake
        and "OptimizedMW_GL-P4_Test.ps1" in root_cmake,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P4R source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P4R source contract passed")
