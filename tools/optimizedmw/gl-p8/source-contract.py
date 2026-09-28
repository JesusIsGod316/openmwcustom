#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
diag = (root / "components/debug/v3diagnostics.hpp").read_text()
p8 = (root / "components/debug/p8dynamicdrawtelemetry.hpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
scene = (root / "components/resource/scenemanager.cpp").read_text()
rig = (root / "components/sceneutil/riggeometry.cpp").read_text()
morph = (root / "components/sceneutil/morphgeometry.cpp").read_text()
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
launcher = (root / "tools/optimizedmw/gl-p7/OptimizedMW_GL-P7_Test.ps1").read_text()
hl1_launcher = (root / "tools/optimizedmw/gl-p8/OptimizedMW_GL-P8_HL1_Test.ps1").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "channels": all(token in diag for token in (
        "OPENMW_P8_DYNAMIC_DRAW_FILE",
        "OPENMW_P8_DYNAMIC_FRAME_FILE",
        "OPENMW_P8_DEFORM_FILE",
        "sceneview0_dynamic,sceneview1_dynamic,dynamic_max")),
    "draw_probe": "TimedDrawCallback" in p8
        and "drawable->drawImplementation(renderInfo)" in p8
        and "durationMs < 0.20" in p8
        and "currentModifiedCount" in p8
        and "dynamicBufferBytes" in p8,
    "scene_dynamic_install": "P8DynamicDrawTelemetry::InstallVisitor" in scene,
    "rig_attribution": 'install(to, "rig_geometry")' in rig
        and 'recordDeformation("rig"' in rig,
    "morph_attribution": 'install(to, "morph_geometry")' in morph
        and 'recordDeformation("morph"' in morph,
    "wait_correlation": "sceneViewDynamic" in engine
        and "getDynamicObjectCount()" in engine
        and "breakdown.dynamicDrawWait" in engine,
    "launcher": all(token in launcher for token in (
        "OPENMW_P8_DYNAMIC_DRAW_FILE",
        "OPENMW_P8_DYNAMIC_FRAME_FILE",
        "OPENMW_P8_DEFORM_FILE",
        "diagnostic_dynamic_draw=true")),
    "hl1_threading_setting": "mOptimizedMWOsgThreadingMode" in cells
        and "makeClampSanitizerInt(0, 3)" in cells
        and "optimizedmw osg threading mode = 0" in defaults,
    "hl1_threading_engine": all(token in engine for token in (
        "mOptimizedMWOsgThreadingMode",
        "DrawThreadPerContext",
        "CullDrawThreadPerContext",
        "SingleThreaded",
        "AutomaticSelection control")),
    "hl1_launcher_modes": all(token in hl1_launcher for token in (
        "CONTROL", "FORCE-DRAWTHREAD", "CULL-DRAW", "SINGLE-THREADED",
        "NO-SHADOWS", "SHADOW-LITE", "GPU-LOWRES", "NO-GROUNDCOVER",
        "SHORT-VISIBILITY", "EAGER-GL", "optimizedmw osg threading mode")),
    "hl1_full_stack": all(token in hl1_launcher for token in (
        "$TerrainResourcePhases='true'",
        "$TerrainSplitVbo='true'",
        "$ParallelActor='true'",
        "$ParallelTerrainCpu='true'",
        "$OsgThreadingMode='0'")),
    "hl1_lean_diagnostics": "OPENMW_P6_TRAVERSAL_FILE" in hl1_launcher
        and "OPENMW_P4_COMPILE_FILE" in hl1_launcher
        and "$env:OPENMW_P8_DYNAMIC_DRAW_FILE=" not in hl1_launcher
        and "$env:OPENMW_P8_DEFORM_FILE=" not in hl1_launcher,
    "historical_hl1_retained": (root / "tools/optimizedmw/gl-p8/START-OptimizedMW-GL-P8-HL1-Test.bat").is_file()
        and (root / "tools/optimizedmw/gl-p8/OptimizedMW_GL-P8_HL1_Test.ps1").is_file(),
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P8 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P8 source contract passed")
