#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
settings_h = (root / "components/settings/categories/groundcover.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()
groundcover = (root / "apps/openmw/mwrender/groundcover.cpp").read_text()
rendering = (root / "apps/openmw/mwrender/renderingmanager.cpp").read_text()
vert = (root / "files/shaders/compatibility/groundcover.vert").read_text()
frag = (root / "files/shaders/compatibility/groundcover.frag").read_text()
launcher = (root / "tools/optimizedmw/gl-p8g/OptimizedMW_GL-P8G_Test.ps1").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "settings_default_off": "mOptimizedMWGpuPath" in settings_h
        and "makeClampSanitizerInt(0, 2)" in settings_h
        and "mOptimizedMWShadowReceive" in settings_h
        and "optimizedmw gpu path = 0" in defaults
        and "optimizedmw shadow receive = true" in defaults,
    "shader_defines": "optimizedmwGroundcoverGpuPath" in rendering
        and "optimizedmwGroundcoverShadowReceive" in rendering,
    "control_euler_retained": "aRotation" in vert
        and "mat4 rotation(in vec3 angle)" in vert
        and "(*eulerRotations)[i] = mInstances[i].mPos.asRotationVec3();" in groundcover,
    "basis_attributes": all(token in groundcover for token in (
        "rotationColumn0", "rotationColumn1", "rotationColumn2",
        "cosZ * cosY + sinX * sinY * sinZ",
        "-sinZ * cosX",
        "-sinY * cosX",
        "VertexAttribDivisor(8, 1)", "VertexAttribDivisor(9, 1)",
        'addBindAttribLocation("aRotation0", 7)',
        'addBindAttribLocation("aRotation1", 8)',
        'addBindAttribLocation("aRotation2", 9)')),
    "basis_shader": all(token in vert for token in (
        "attribute vec3 aRotation0",
        "attribute vec3 aRotation1",
        "attribute vec3 aRotation2",
        "mat3 optimizedmwInstanceRotation()",
        "instanceRotation * gl_Vertex.xyz",
        "instanceRotation * gl_Normal.xyz",
        "gl_MultiTexCoord7.xyz * instanceRotation")),
    "early_reject": "instanceBaseViewPos" in vert
        and vert.index("instanceBaseViewPos") < vert.index("groundcoverDisplacement(worldPos.xyz"),
    "wind_probe": "@optimizedmwGroundcoverGpuPath >= 2" in vert
        and "two middle harmonics" in vert,
    "shadow_probe": "@optimizedmwGroundcoverShadowReceive" in vert
        and "@optimizedmwGroundcoverShadowReceive" in frag
        and "shadowing = 1.0;" in frag,
    "launcher": all(token in launcher for token in (
        "CONTROL", "SAFE-BASIS", "FAST-WIND", "SAFE-NO-POINT",
        "SAFE-NO-GRASS-SHADOWS", "SAFE-SHADOW-LITE",
        "optimizedmw gpu path", "optimizedmw shadow receive")),
    "launcher_full_stack": all(token in launcher for token in (
        "$TerrainResourcePhases='true'", "$TerrainSplitVbo='true'",
        "$ParallelActor='true'", "$ParallelTerrainCpu='true'",
        "$OsgThreadingMode='0'")),
    "packaged": "START-OptimizedMW-GL-P8G-Test.bat" in cmake
        and "OptimizedMW_GL-P8G_Test.ps1" in cmake,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("GL-P8G source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P8G source contract passed")
