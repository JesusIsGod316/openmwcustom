import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.10 post-transform match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.10 post-transform preload patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.10 design contract
#
# Runtime validation established two independent facts:
#   1. V3.9's compile=true startup frontload removes almost all strong-batching
#      optimization work from ordinary traversal.
#   2. V3.8 mode 47's large steady draw/GPU win disappeared when
#      VERTEX_POSTTRANSFORM was removed, even when strong merged chunks remained
#      cached. Post-transform index/vertex-cache ordering is therefore promoted
#      back into the PRELOAD path only.
#
# The switch below is deliberately orthogonal to V3.9's optimizer selector so a
# clean Mode-56 A/B can change exactly one behavior. When enabled:
#   - compile=true + batching>=2 gets VERTEX_POSTTRANSFORM + shared-state cleanup;
#   - compile=false remains governed by V3.9's conservative mode-1 merge-only
#     emergency fallback;
#   - VERTEX_PRETRANSFORM is NEVER requested by this V3.10 override;
#   - all existing eligibility, material/PBR, animation, alpha, LOD, refnum,
#     occlusion and shadow rules remain unchanged.
#
# A deeper Lua audit was also completed before this patch. The tempting shortcut
# of skipping ensureLoaded() for serialized idle timers is NOT enabled: active
# local containers are subsequently updated in the same Lua worker iteration and
# may require materialization/top-level/onLoad semantics. V3.10 keeps Lua behavior
# unchanged until a semantics-preserving materialization strategy is proven.
# -----------------------------------------------------------------------------

replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV39ProactiveResidencyMode{ mIndex, "V3", "v3.9 proactive residency mode",
            makeClampSanitizerInt(0, 3) };''',
    '''        SettingValue<int> mV39ProactiveResidencyMode{ mIndex, "V3", "v3.9 proactive residency mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<bool> mV310PreloadPostTransform{ mIndex, "V3", "v3.10 preload post-transform" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# Earlier cache-only render-graph reclamation while adapter pressure is Soft.
# 0=V3.8 behavior, 1=gentle, 2=balanced, 3=aggressive.
v3.9 proactive residency mode = 0

[Cells]''',
    '''# Earlier cache-only render-graph reclamation while adapter pressure is Soft.
# 0=V3.8 behavior, 1=gentle, 2=balanced, 3=aggressive.
v3.9 proactive residency mode = 0

# V3.10 preload-only locality promotion. When true, strong ObjectPaging chunks
# built through compile=true startup/background preload receive post-transform
# vertex-cache optimization and shared-state compaction. Synchronous compile=false
# misses still use V3.9's conservative merge-only fallback. This switch never
# enables VERTEX_PRETRANSFORM.
v3.10 preload post-transform = false

[Cells]''',
)

# Insert the V3.10 guard AFTER V3.9 preload-priority has already converted the
# configured optimizer into an effective optimizer. This makes compile=false
# fallback authoritative and prevents V3.10 from leaking expensive reordering
# back onto an on-demand traversal miss.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const int v39BatchOptimizerMode
                = (static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile)
                ? 1
                : v39ConfiguredBatchOptimizerMode;

            if (v39BatchOptimizerMode == 0)''',
    '''            const int v39BatchOptimizerMode
                = (static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile)
                ? 1
                : v39ConfiguredBatchOptimizerMode;
            const bool v310PreloadPostTransform
                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)
                && compile && v38BatchingMode >= 2;

            if (v39BatchOptimizerMode == 0 && !v310PreloadPostTransform)''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            else if (v39BatchOptimizerMode >= 3 && v38BatchingMode >= 2)
            {
                // Retain only the cheaper locality pass. VERTEX_PRETRANSFORM is
                // deliberately excluded from all V3.9 optimized policies.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }''',
    '''            else if ((v39BatchOptimizerMode >= 3 || v310PreloadPostTransform) && v38BatchingMode >= 2)
            {
                // V3.10 may explicitly promote this locality pass on preload work.
                // The V3.10 override never requests VERTEX_PRETRANSFORM.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const bool v39ShareState = v39BatchOptimizerMode >= 2;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);''',
    '''            const bool v39ShareState = v39BatchOptimizerMode >= 2 || v310PreloadPostTransform;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);''',
)

# -----------------------------------------------------------------------------
# Compact V3.10 runtime matrix.
# 59 is an exact V3.9 Mode-56 reference.
# 60 changes ONLY the new preload-posttransform switch: highest-value first test.
# 61 layers the already visually-clean 5px far-shadow threshold on the same core.
# 62 spends additional startup time on V3.9's 5x5 frontload to test coverage.
# No V3.10 profile enables V3.9 proactive whole-render-graph expiry; Mode56/57
# proved that deleting ~1000 prepared chunks saved only ~41 MB of adapter memory.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V39ProactiveResidencyMode = '0'
$RendererProfiling''',
    '''$V39ProactiveResidencyMode = '0'
$V310PreloadPostTransform = 'false'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
Write-Host ' 55 = V3.9 V3.8-safe reference (Mode 46 equivalent)'
Write-Host ' 56 = V3.9 frontloaded strong batching'
Write-Host ' 57 = V3.9 combined candidate'
Write-Host ' 58 = V3.9 aggressive frontload / batching / residency'
do { $choice = Read-Host 'Enter 1 through 58' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58'))''',
    '''Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
Write-Host ' 55 = V3.9 V3.8-safe reference (Mode 46 equivalent)'
Write-Host ' 56 = V3.9 frontloaded strong batching'
Write-Host ' 57 = V3.9 combined candidate'
Write-Host ' 58 = V3.9 aggressive frontload / batching / residency'
Write-Host ' 59 = V3.10 V3.9 Mode-56 reference'
Write-Host ' 60 = V3.10 preload post-transform 3x3 (FIRST TEST)'
Write-Host ' 61 = V3.10 combined post-transform + 5px far shadow'
Write-Host ' 62 = V3.10 post-transform 5x5 startup coverage'
do { $choice = Read-Host 'Enter 1 through 62' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '58' { $Experiment = 'v39-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '3'; $V39ProactiveResidencyMode = '3' }
}''',
    '''    '58' { $Experiment = 'v39-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '3'; $V39ProactiveResidencyMode = '3' }
    '59' { $Experiment = 'v310-v39-mode56-reference'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1' }
    '60' { $Experiment = 'v310-posttransform-3x3'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310PreloadPostTransform = 'true' }
    '61' { $Experiment = 'v310-combined-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310PreloadPostTransform = 'true' }
    '62' { $Experiment = 'v310-posttransform-5x5'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '1'; $V310PreloadPostTransform = 'true' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v39_proactive_residency_mode=$V39ProactiveResidencyMode",
    "shadow_distance=$ShadowDistance",''',
    '''    "v39_proactive_residency_mode=$V39ProactiveResidencyMode",
    "v310_preload_posttransform=$V310PreloadPostTransform",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.9 proactive residency mode' $V39ProactiveResidencyMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.9 proactive residency mode' $V39ProactiveResidencyMode
    Set-IniValue $SettingsPath 'V3' 'v3.10 preload post-transform' $V310PreloadPostTransform
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.10 preload-only post-transform promotion patched successfully.")
