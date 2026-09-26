#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
diag = (root / "components/debug/v3diagnostics.hpp").read_text()
p8 = (root / "components/debug/p8dynamicdrawtelemetry.hpp").read_text()
engine = (root / "apps/openmw/engine.cpp").read_text()
scene = (root / "components/resource/scenemanager.cpp").read_text()
rig = (root / "components/sceneutil/riggeometry.cpp").read_text()
morph = (root / "components/sceneutil/morphgeometry.cpp").read_text()
launcher = (root / "tools/optimizedmw/gl-p7/OptimizedMW_GL-P7_Test.ps1").read_text()

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
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P8 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P8 source contract passed")
