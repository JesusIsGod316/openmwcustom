import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.8 compile-pacing match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.8 compile-pacing patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Incremental GL compile pacing for traversal smoothness.
#
# OSG computes available compile/delete time from the ICO target frame time and
# elapsed frame time. On this project's ~22-23ms exterior frames, a 60fps target
# normally collapses to OSG's minimum budget. These modes let already-preloaded
# paged content receive a predictable amount of GL preparation before first use,
# while capping the number of objects compiled in one frame.
#
# 0 = exact existing OpenMW behavior.
# 1 = conservative frame-pacing: original target, lower object cap and lower
#     conservative ratio to reduce individual compile bursts.
# 2 = balanced: at most 45fps compile target, 8 objects/frame.
# 3 = aggressive preparation: at most 36fps compile target, 12 objects/frame,
#     more of measured spare time available to compilation.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include "renderingmanager.hpp"

#include <cstdlib>''',
    '''#include "renderingmanager.hpp"

#include <algorithm>
#include <cstdlib>''',
)

replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV38FarShadowMode{ mIndex, "V3", "v3.8 far shadow mode",
            makeClampSanitizerInt(0, 3) };''',
    '''        SettingValue<int> mV38FarShadowMode{ mIndex, "V3", "v3.8 far shadow mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38CompilePacingMode{ mIndex, "V3", "v3.8 compile pacing mode",
            makeClampSanitizerInt(0, 3) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.8 far shadow mode = 0

[Cells]''',
    '''v3.8 far shadow mode = 0

# Incremental OpenGL compile pacing for newly paged/preloaded render objects.
# 0=OpenMW/default, 1=conservative burst cap, 2=balanced preparation, 3=aggressive preparation.
v3.8 compile pacing mode = 0

[Cells]''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)
        {
            mViewer->setIncrementalCompileOperation(new osgUtil::IncrementalCompileOperation);
            mViewer->getIncrementalCompileOperation()->setTargetFrameRate(Settings::cells().mTargetFramerate);
        }''',
    '''        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)
        {
            osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico = new osgUtil::IncrementalCompileOperation;

            const double configuredTarget = static_cast<double>(Settings::cells().mTargetFramerate);
            double compileTarget = configuredTarget;
            unsigned int maxObjectsPerFrame = ico->getMaximumNumOfObjectsToCompilePerFrame();
            double conservativeRatio = 0.5;

            switch (static_cast<int>(Settings::cells().mV38CompilePacingMode))
            {
                case 1:
                    // Keep the user's target-frame-time policy, but limit how many
                    // potentially expensive GL objects may be attempted in one frame.
                    maxObjectsPerFrame = 6;
                    conservativeRatio = 0.35;
                    break;
                case 2:
                    // Allow modest spare-time preparation on a mid-40fps workload.
                    compileTarget = std::min(configuredTarget, 45.0);
                    maxObjectsPerFrame = 8;
                    conservativeRatio = 0.5;
                    break;
                case 3:
                    // Spend more otherwise-unused headroom preparing paged VBO/state
                    // before first visibility. The time budget remains bounded by ICO.
                    compileTarget = std::min(configuredTarget, 36.0);
                    maxObjectsPerFrame = 12;
                    conservativeRatio = 0.6;
                    break;
                default:
                    break;
            }

            ico->setTargetFrameRate(compileTarget);
            if (static_cast<int>(Settings::cells().mV38CompilePacingMode) > 0)
            {
                ico->setMaximumNumOfObjectsToCompilePerFrame(maxObjectsPerFrame);
                ico->setConservativeTimeRatio(conservativeRatio);
            }
            mViewer->setIncrementalCompileOperation(ico);
        }''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V38FarShadowMode = '0'
$RendererProfiling''',
    '''$V38FarShadowMode = '0'
$V38CompilePacingMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 51 = V3.8 far-shadow aggressive (5px)'
do { $choice = Read-Host 'Enter 1 through 51' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51'))''',
    '''Write-Host ' 51 = V3.8 far-shadow aggressive (5px)'
Write-Host ' 52 = V3.8 compile pacing conservative'
Write-Host ' 53 = V3.8 compile pacing balanced'
Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
do { $choice = Read-Host 'Enter 1 through 54' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3' }''',
    '''    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '2'; $V38CompilePacingMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3' }''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '51' { $Experiment = 'v38-shadow-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '3' }
}''',
    '''    '51' { $Experiment = 'v38-shadow-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '3' }
    '52' { $Experiment = 'v38-compile-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '1' }
    '53' { $Experiment = 'v38-compile-balanced'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '2' }
    '54' { $Experiment = 'v38-compile-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '3' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v38_far_shadow_mode=$V38FarShadowMode",
    "shadow_distance=$ShadowDistance",''',
    '''    "v38_far_shadow_mode=$V38FarShadowMode",
    "v38_compile_pacing_mode=$V38CompilePacingMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 far shadow mode' $V38FarShadowMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 far shadow mode' $V38FarShadowMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' $V38CompilePacingMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.8 incremental GL compile-pacing profiles patched successfully.")
