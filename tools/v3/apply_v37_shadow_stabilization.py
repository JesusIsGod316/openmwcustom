import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.7 shadow-stabilization match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.7 shadow stabilization patched {rel} ({count} match(es))")


# Default-off far-cascade texel stabilization. The adjustment is deliberately
# limited to an orthographic far cascade and never exceeds half a texel on either
# axis. Near/middle cascades, caster masks, actor/player updates and shadow-map
# resolution are unchanged.
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<float> mV37ResourceSweepSeconds{ mIndex, "V3", "v3.7 resource cache sweep seconds",
            makeClampSanitizerFloat(0.5, 60) };''',
    '''        SettingValue<float> mV37ResourceSweepSeconds{ mIndex, "V3", "v3.7 resource cache sweep seconds",
            makeClampSanitizerFloat(0.5, 60) };
        SettingValue<bool> mV37StabilizeFarCascade{ mIndex, "V3", "v3.7 stabilize far shadow cascade" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.7 relaxed resource cache sweep = false
v3.7 resource cache sweep seconds = 5.0

[Cells]''',
    '''v3.7 relaxed resource cache sweep = false
v3.7 resource cache sweep seconds = 5.0

# Snap only the far orthographic cascade to its actual texture texel grid.
# This is a visual-stability experiment, not whole-map reuse, and is off until A/B tested.
v3.7 stabilize far shadow cascade = false

[Cells]''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        void setV36FarCasterMinimumPixels(float pixels) { _v36FarCasterMinimumPixels = std::max(0.f, pixels); }
        float getV36FarCasterMinimumPixels() const { return _v36FarCasterMinimumPixels; }''',
    '''        void setV36FarCasterMinimumPixels(float pixels) { _v36FarCasterMinimumPixels = std::max(0.f, pixels); }
        float getV36FarCasterMinimumPixels() const { return _v36FarCasterMinimumPixels; }
        void setV37StabilizeFarCascade(bool enabled) { _v37StabilizeFarCascade = enabled; }''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        bool                                    _v36AsyncGpuProfiler = false;
        float                                   _v36FarCasterMinimumPixels = 0.f;''',
    '''        bool                                    _v36AsyncGpuProfiler = false;
        float                                   _v36FarCasterMinimumPixels = 0.f;
        bool                                    _v37StabilizeFarCascade = false;''',
)

replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV36AsyncGpuProfiler(Settings::cells().mV36AsyncGpuProfiler);
        mShadowTechnique->setV36FarCasterMinimumPixels(Settings::V36Profile::farCasterMinimumPixels());''',
    '''        mShadowTechnique->setV36AsyncGpuProfiler(Settings::cells().mV36AsyncGpuProfiler);
        mShadowTechnique->setV36FarCasterMinimumPixels(Settings::V36Profile::farCasterMinimumPixels());
        mShadowTechnique->setV37StabilizeFarCascade(Settings::cells().mV37StabilizeFarCascade);''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''#include <sstream>
#include <vector>''',
    '''#include <cmath>
#include <sstream>
#include <vector>''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''            else
                cropShadowCameraToMainFrustum(frustum, camera, reducedNear, reducedFar, extraPlanes);

            bool v33ReuseFarCascade = false;''',
    '''            else
                cropShadowCameraToMainFrustum(frustum, camera, reducedNear, reducedFar, extraPlanes);

            if (_v37StabilizeFarCascade && sm_i + 1 == numShadowMapsPerLight && camera->getViewport())
            {
                const osg::Matrixd v37Projection = camera->getProjectionMatrix();
                const bool v37Orthographic = v37Projection(0, 3) == 0.0 && v37Projection(1, 3) == 0.0
                    && v37Projection(2, 3) == 0.0;
                if (v37Orthographic)
                {
                    const osg::Matrixd v37ViewProjection = camera->getViewMatrix() * v37Projection;
                    const osg::Vec3d v37Origin = osg::Vec3d(0.0, 0.0, 0.0) * v37ViewProjection;
                    const double v37HalfWidth = static_cast<double>(camera->getViewport()->width()) * 0.5;
                    const double v37HalfHeight = static_cast<double>(camera->getViewport()->height()) * 0.5;
                    if (v37HalfWidth > 0.0 && v37HalfHeight > 0.0)
                    {
                        const double v37SnappedX = std::round(v37Origin.x() * v37HalfWidth) / v37HalfWidth;
                        const double v37SnappedY = std::round(v37Origin.y() * v37HalfHeight) / v37HalfHeight;
                        const double v37OffsetX = v37SnappedX - v37Origin.x();
                        const double v37OffsetY = v37SnappedY - v37Origin.y();
                        camera->setProjectionMatrix(v37Projection
                            * osg::Matrixd::translate(osg::Vec3d(v37OffsetX, v37OffsetY, 0.0)));
                    }
                }
            }

            bool v33ReuseFarCascade = false;''',
)

# Launcher: keep stabilization out of the normal V3.7 candidate until visual A/B
# testing. Choice 38 uses the validated V3.6 profile and fixes shadow distance at
# 6144 so the far cascade is stressed where the optimization is intended to help.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V37GpuMemoryManagement = 'false'
$RendererProfiling''',
    '''$V37GpuMemoryManagement = 'false'
$V37StabilizeFarCascade = 'false'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 37 = V3.7 adapter-aware speculative preload admission isolated'
do { $choice = Read-Host 'Enter 1 through 37' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37'))''',
    '''Write-Host ' 37 = V3.7 adapter-aware speculative preload admission isolated'
Write-Host ' 38 = V3.7 far-cascade texel stabilization at 6144 shadow distance'
do { $choice = Read-Host 'Enter 1 through 38' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '37' { $Experiment = 'v37-vram-admission-isolated'; $V36PerformanceProfile = 'true'; $V37GpuMemoryManagement = 'true' }
}''',
    '''    '37' { $Experiment = 'v37-vram-admission-isolated'; $V36PerformanceProfile = 'true'; $V37GpuMemoryManagement = 'true' }
    '38' { $Experiment = 'v37-far-stabilization-6144'; $V36PerformanceProfile = 'true'; $V37StabilizeFarCascade = 'true'; $ShadowDistance = '6144' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v37_gpu_memory_management=$V37GpuMemoryManagement",
    "shadow_distance=$ShadowDistance",''',
    '''    "v37_gpu_memory_management=$V37GpuMemoryManagement",
    "v37_stabilize_far_cascade=$V37StabilizeFarCascade",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.7 resource cache sweep seconds' $V37ResourceSweepSeconds
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.7 resource cache sweep seconds' $V37ResourceSweepSeconds
    Set-IniValue $SettingsPath 'V3' 'v3.7 stabilize far shadow cascade' $V37StabilizeFarCascade
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.7 far-cascade texel stabilization experiment patch completed successfully.")
