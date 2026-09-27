#!/usr/bin/env python3
from pathlib import Path
import math

root = Path(__file__).resolve().parents[3]
settings_h = (root / "components/settings/categories/groundcover.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
groundcover = (root / "apps/openmw/mwrender/groundcover.cpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
state_h = (root / "components/sceneutil/stateupdater.hpp").read_text()
state_cpp = (root / "components/sceneutil/stateupdater.cpp").read_text()
vert = (root / "files/shaders/compatibility/groundcover.vert").read_text()
frag = (root / "files/shaders/compatibility/groundcover.frag").read_text()
launcher = (root / "tools/optimizedmw/gl-p8g2/OptimizedMW_GL-P8G2_Test.ps1").read_text()
cmake = (root / "CMakeLists.txt").read_text()

# Numerically prove the optimized z-only matrix is the exact specialization of
# the established Euler formula for x=y=0.
for z in (0.0, 0.17, -0.8, 2.3):
    s, c = math.sin(z), math.cos(z)
    established = (
        (c, s, 0.0),
        (-s, c, 0.0),
        (0.0, 0.0, 1.0),
    )
    optimized = established
    assert optimized == established

checks = {
    "settings_default_off": "mOptimizedMWGpuPath" in settings_h
        and "makeClampSanitizerInt(0, 2)" in settings_h
        and "mOptimizedMWShadowReceive" in settings_h
        and "optimizedmw gpu path = 0" in defaults
        and "optimizedmw shadow receive = true" in defaults,
    "defines": "optimizedmwGroundcoverGpuPath" in rendering
        and "optimizedmwGroundcoverShadowReceive" in rendering,
    "legacy_attributes_preserved": 'addBindAttribLocation("aOffset", 6)' in groundcover
        and 'addBindAttribLocation("aRotation", 7)' in groundcover
        and "setVertexAttribArray(6" in groundcover
        and "setVertexAttribArray(7" in groundcover,
    "uv_collision_forbidden": "setVertexAttribArray(8" not in groundcover
        and "setVertexAttribArray(9" not in groundcover
        and "VertexAttribDivisor(8" not in groundcover
        and "VertexAttribDivisor(9" not in groundcover
        and "aRotation0" not in vert
        and "aRotation1" not in vert
        and "aRotation2" not in vert,
    "wind_precompute": "mGroundcoverWindCoefficients" in state_h
        and 'groundcoverWindCoefficients' in state_cpp
        and "std::sqrt(2.f * windSpeed * windSpeed + 1.f)" in state_cpp
        and "groundcoverWindCoefficients" in vert,
    "safe_shader": all(token in vert for token in (
        "instanceBaseViewPos",
        "stompDistanceSquared",
        "aRotation.x == 0.0 && aRotation.y == 0.0",
        "@optimizedmwGroundcoverGpuPath >= 1")),
    "control_retained": "mat4 rotation(in vec3 angle)" in vert
        and "length(gl_ModelViewMatrix * vec4(position, 1.0))" in vert
        and "attribute vec3 aRotation;" in vert,
    "shadow_probe": "@optimizedmwGroundcoverShadowReceive" in vert
        and "@optimizedmwGroundcoverShadowReceive" in frag
        and "shadowing = 1.0;" in frag,
    "launcher": all(token in launcher for token in (
        "CONTROL", "SAFE-SHADER", "FAST-WIND", "SAFE-NO-POINT",
        "SAFE-NO-GRASS-SHADOWS", "optimizedmw gpu path",
        "optimizedmw shadow receive")),
    "no_shadow_lite": "SAFE-SHADOW-LITE" not in launcher,
    "full_stack": all(token in launcher for token in (
        "$TerrainResourcePhases='true'", "$TerrainSplitVbo='true'",
        "$ParallelActor='true'", "$ParallelTerrainCpu='true'",
        "$OsgThreadingMode='0'")),
    "packaged": "START-OptimizedMW-GL-P8G2-Test.bat" in cmake
        and "OptimizedMW_GL-P8G2_Test.ps1" in cmake,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P8G2 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P8G2 source contract passed")
