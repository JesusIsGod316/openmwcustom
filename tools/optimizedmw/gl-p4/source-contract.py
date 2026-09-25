#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
compile_h = (root / "components/resource/openmwcompileoperation.hpp").read_text()
compile_cpp = (root / "components/resource/openmwcompileoperation.cpp").read_text()
policy = (root / "components/resource/p4compilepolicy.hpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
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
        "mOptimizedMWCompileSchedulerMaxObjectsPerFrame")),
    "production_build": "openmwcompileoperation" in cmake,
    "control_exact_path": "new osgUtil::IncrementalCompileOperation" in rendering
        and "new Resource::OpenMWIncrementalCompileOperation" in rendering,
    "handoff_feedback": "publishRenderingTraversalMs" in engine
        and "mOptimizedMWCompileSchedulerMode) >= 2" in engine,
    "cost_aware": "predictedMs" in compile_cpp
        and "estimatedTimeForCompile" in compile_cpp
        and "observe(selectedKind, actualMs)" in compile_cpp,
    "queue_age_guard": "forced_by_queue_age" in compile_cpp
        and "mOptimizedMWCompileSchedulerMaxQueueAgeFrames" in compile_cpp,
    "class_priority": "V321CompileClass::ObjectPaging" in compile_cpp
        and "V321CompileClass::Terrain" in compile_cpp,
    "separate_delete_budget": "separate_delete_budget" in compile_cpp
        and "mOptimizedMWCompileSchedulerDeleteBudgetMs" in compile_cpp,
    "nonblocking_diagnostics": "OPENMW_P4_COMPILE_FILE" in diagnostics
        and "DiagnosticWriterHub" in diagnostics,
    "policy_handoff_suppression": "mSuppressedByHandoff" in policy
        and "state.mCreditMs *= 0.25" in policy,
    "launcher_matrix": all(token in launcher for token in (
        "CONTROL-B1", "CONTROL-B1-C", "P4A-B1", "P4B-B1",
        "P4B-B1-C", "P4B-B1-C-D", "P4B-B1-C-COMPLETION",
        "OPENMW_P4_COMPILE_FILE", "optimizedmw compile scheduler mode")),
    "launcher_packaged": "START-OptimizedMW-GL-P4-Test.bat" in root_cmake
        and "OptimizedMW_GL-P4_Test.ps1" in root_cmake,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P4 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P4 source contract passed")
