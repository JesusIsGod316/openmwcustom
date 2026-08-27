import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.8 shadow/traversal match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.8 shadow/traversal patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Graded far-cascade projected-size pruning.
#
# Mode 0 preserves the proven V3.7 2-pixel profile exactly.
# Modes 1-3 deliberately expose progressively more aggressive far-cascade-only
# culling. Near/mid cascades, shadow-map resolution, dynamic caster semantics and
# receiver shading are untouched. This keeps visual risk isolated and reversible.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV38GpuResidencyMode{ mIndex, "V3", "v3.8 gpu residency mode",
            makeClampSanitizerInt(0, 3) };''',
    '''        SettingValue<int> mV38GpuResidencyMode{ mIndex, "V3", "v3.8 gpu residency mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38FarShadowMode{ mIndex, "V3", "v3.8 far shadow mode",
            makeClampSanitizerInt(0, 3) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.8 gpu residency mode = 0

[Cells]''',
    '''v3.8 gpu residency mode = 0

# Additional far-cascade projected-size pruning layered on the proven V3.7 2px baseline.
# 0=proven 2px, 1=2.5px conservative, 2=3.5px moderate, 3=5px aggressive.
# Only the farthest shadow cascade is affected.
v3.8 far shadow mode = 0

[Cells]''',
)

replace_exact(
    "components/settings/v36profile.hpp",
    '''    inline float farCasterMinimumPixels()
    {
        // V3.6/6144 A-B testing validated 2 px with no user-visible artifacts.
        // The V3.7 disable switch is deliberately independent so this proven
        // optimization can still be isolated without disabling the rest of the profile.
        if (enabled())
            return static_cast<bool>(cells().mV37DisableFarCasterPruning) ? 0.f : 2.f;
        return static_cast<float>(cells().mV36FarCasterMinimumPixels);
    }''',
    '''    inline float farCasterMinimumPixels()
    {
        // V3.6/6144 A-B testing validated 2 px with no user-visible artifacts.
        // V3.8 extends only the farthest cascade with opt-in graded thresholds.
        if (enabled())
        {
            if (static_cast<bool>(cells().mV37DisableFarCasterPruning))
                return 0.f;

            switch (static_cast<int>(cells().mV38FarShadowMode))
            {
                case 1:
                    return 2.5f;
                case 2:
                    return 3.5f;
                case 3:
                    return 5.f;
                default:
                    return 2.f;
            }
        }
        return static_cast<float>(cells().mV36FarCasterMinimumPixels);
    }''',
)

# -----------------------------------------------------------------------------
# Post-batch shared-state compaction.
#
# MergeGeometry can leave equivalent StateSets/state attributes on newly merged
# geometry. SceneManager::shareState() uses OpenMW's existing SharedStateManager
# and its internal mutex, so this is the engine's already-proven thread-safe path
# rather than a new interning table. Limit it to moderate/aggressive batching to
# avoid adding worker cost to conservative A/B runs.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            optimizer.optimize(mergeGroup, options);

            group->addChild(mergeGroup);''',
    '''            optimizer.optimize(mergeGroup, options);

            if (v38BatchingMode >= 2)
                mSceneManager->shareState(mergeGroup);

            group->addChild(mergeGroup);''',
)

# -----------------------------------------------------------------------------
# Launcher integration. 39 remains the clean V3.8 baseline. 40-45 stay isolated
# batching/residency modes. 46-48 become true combined conservative/moderate/
# aggressive profiles. 49-51 isolate the new shadow thresholds without another
# engine build.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V38GpuResidencyMode = '0'
$RendererProfiling''',
    '''$V38GpuResidencyMode = '0'
$V38FarShadowMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 48 = V3.8 traversal combined aggressive'
do { $choice = Read-Host 'Enter 1 through 48' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48'))''',
    '''Write-Host ' 48 = V3.8 traversal combined aggressive'
Write-Host ' 49 = V3.8 far-shadow conservative (2.5px)'
Write-Host ' 50 = V3.8 far-shadow moderate (3.5px)'
Write-Host ' 51 = V3.8 far-shadow aggressive (5px)'
do { $choice = Read-Host 'Enter 1 through 51' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3' }
}''',
    '''    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3' }
    '49' { $Experiment = 'v38-shadow-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '1' }
    '50' { $Experiment = 'v38-shadow-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '2' }
    '51' { $Experiment = 'v38-shadow-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '3' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v38_gpu_residency_mode=$V38GpuResidencyMode",
    "shadow_distance=$ShadowDistance",''',
    '''    "v38_gpu_residency_mode=$V38GpuResidencyMode",
    "v38_far_shadow_mode=$V38FarShadowMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 gpu residency mode' $V38GpuResidencyMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 gpu residency mode' $V38GpuResidencyMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 far shadow mode' $V38FarShadowMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.8 graded far-shadow pruning and post-batch shared-state compaction patched successfully.")
