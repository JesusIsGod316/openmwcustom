import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.7 hitch-path match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.7 hitch-path patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Runtime switches for the next hitch-focused experiments. They remain default
# off so all V3.6 comparison choices preserve their historical behavior.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<bool> mV37DisableFarCasterPruning{
            mIndex, "V3", "v3.7 disable far caster pruning" };
        SettingValue<bool> mV37ActiveEventFastPath{ mIndex, "V3", "v3.7 active event fast path" };''',
    '''        SettingValue<bool> mV37DisableFarCasterPruning{
            mIndex, "V3", "v3.7 disable far caster pruning" };
        SettingValue<bool> mV37ActiveEventFastPath{ mIndex, "V3", "v3.7 active event fast path" };
        SettingValue<bool> mV37CompanionKeyframePreload{
            mIndex, "V3", "v3.7 companion keyframe preload" };
        SettingValue<bool> mV37RelaxedResourceSweep{
            mIndex, "V3", "v3.7 relaxed resource cache sweep" };
        SettingValue<float> mV37ResourceSweepSeconds{ mIndex, "V3", "v3.7 resource cache sweep seconds",
            makeClampSanitizerFloat(0.5, 60) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# Semantics-preserving bulk OnActive dispatch experiment. Default off until A/B tested.
v3.7 active event fast path = false

[Cells]''',
    '''# Semantics-preserving bulk OnActive dispatch experiment. Default off until A/B tested.
v3.7 active event fast path = false

# Preload a companion .kf for any preloaded NIF when one exists, not only the legacy x-prefixed path.
# This keeps immutable keyframe parsing on the existing preload worker instead of the activation path.
v3.7 companion keyframe preload = false

# Overdrive keeps resources for long periods, so sweeping every second mostly burns worker/cache-lock time.
# This experiment only changes the sweep cadence; expiry semantics and cache contents are otherwise unchanged.
v3.7 relaxed resource cache sweep = false
v3.7 resource cache sweep seconds = 5.0

[Cells]''',
)


# -----------------------------------------------------------------------------
# Loaded-container empty-handler fast path.
#
# This is unconditional because it is semantics preserving: if the container is
# already LoadedData and the registered handler list is empty, ensureLoaded()
# cannot materialize anything and there is no callback to execute. UnloadedData
# still falls through to ensureLoaded(), preserving first-load/onLoad semantics.
# -----------------------------------------------------------------------------
replace_exact(
    "components/lua/scriptscontainer.hpp",
    '''        void callEngineHandlers(EngineHandlerList& handlers, const Args&... args)
        {
            ensureLoaded();
            for (Handler& handler : handlers.mList)''',
    '''        void callEngineHandlers(EngineHandlerList& handlers, const Args&... args)
        {
            if (handlers.mList.empty() && std::holds_alternative<LoadedData>(mData))
                return;
            ensureLoaded();
            for (Handler& handler : handlers.mList)''',
)


# -----------------------------------------------------------------------------
# Companion keyframe preloading.
#
# Upstream only preloads .kf data for x-prefixed NIFs. Animation::addAnimSource
# can request a same-name .kf for any NIF, so actor activation can still pay the
# parse cost even though the model itself was preloaded. The experiment broadens
# that immutable KeyframeManager warm-up to every preloaded NIF while preserving
# the old x-prefixed behavior when the switch is off. Duplicate paths are
# suppressed inside one preload item; repeated scene meshes remain untouched so
# the prepared-static-instance experiment keeps its existing multiplicity.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/terrain/view.hpp>''',
    '''#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''            VFS::Path::Normalized mesh;
            VFS::Path::Normalized kfname;
            for (VFS::Path::NormalizedView path : mMeshes)''',
    '''            VFS::Path::Normalized mesh;
            VFS::Path::Normalized kfname;
            std::set<VFS::Path::Normalized> v37PreloadedKeyframes;
            const bool v37CompanionKeyframePreload
                = static_cast<bool>(Settings::cells().mV37CompanionKeyframePreload);
            for (VFS::Path::NormalizedView path : mMeshes)''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''                    constexpr VFS::Path::ExtensionView nif("nif");
                    if (Misc::getFileName(mesh).starts_with('x') && mesh.extension() == nif)
                    {
                        kfname = mesh;
                        constexpr VFS::Path::ExtensionView kf("kf");
                        kfname.changeExtension(kf);
                        if (vfs.exists(kfname))
                            mPreloadedObjects.insert(mKeyframeManager->get(kfname));
                    }''',
    '''                    constexpr VFS::Path::ExtensionView nif("nif");
                    const bool v37CheckKeyframe = mesh.extension() == nif
                        && (Misc::getFileName(mesh).starts_with('x') || v37CompanionKeyframePreload);
                    if (v37CheckKeyframe)
                    {
                        kfname = mesh;
                        constexpr VFS::Path::ExtensionView kf("kf");
                        kfname.changeExtension(kf);
                        if (vfs.exists(kfname) && v37PreloadedKeyframes.insert(kfname).second)
                            mPreloadedObjects.insert(mKeyframeManager->get(kfname));
                    }''',
)


# -----------------------------------------------------------------------------
# Relax the ResourceSystem sweep cadence when explicitly selected. V3 Overdrive
# uses very long expiry windows, so one-second sweeps normally cannot evict most
# retained objects yet still walk manager caches and contend on their locks. The
# actual expiry delays are unchanged and the normal one-second path is preserved
# unless this experiment is enabled.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        if (timestamp - mLastResourceCacheUpdate > 1.0 && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {''',
    '''        const double v37ResourceSweepSeconds
            = static_cast<bool>(Settings::cells().mV37RelaxedResourceSweep)
            ? static_cast<double>(Settings::cells().mV37ResourceSweepSeconds)
            : 1.0;
        if (timestamp - mLastResourceCacheUpdate > v37ResourceSweepSeconds
            && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {''',
)


# -----------------------------------------------------------------------------
# Extend the unified launcher with clean V3.7 A/B choices. Older V3.6 choices do
# not enable any of these new experimental switches, which keeps comparison data
# interpretable on the same executable.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V36FarCasterMinimumPixels = '0.0'
$V36Attribution = $false
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }''',
    '''$V36FarCasterMinimumPixels = '0.0'
$V36Attribution = $false
$V37ActiveEventFastPath = 'false'
$V37CompanionKeyframePreload = 'false'
$V37RelaxedResourceSweep = 'false'
$V37ResourceSweepSeconds = '5.0'
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 31 = V3.6 hitch combined (normal + deep hitch attribution)'
Write-Host ' 32 = V3.6 full diagnostic combined'
do { $choice = Read-Host 'Enter 1 through 32' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32'))''',
    '''Write-Host ' 31 = V3.6 hitch combined (normal + deep hitch attribution)'
Write-Host ' 32 = V3.6 full diagnostic combined'
Write-Host ''
Write-Host 'V3.7 hitch-path experiments:' -ForegroundColor Magenta
Write-Host ' 33 = V3.7 normal candidate (V3.6 profile + active-event fast path + keyframe preload + relaxed cache sweep)'
Write-Host ' 34 = V3.7 active-event fast path isolated'
Write-Host ' 35 = V3.7 companion-keyframe preload isolated + hitch attribution'
Write-Host ' 36 = V3.7 hitch combined + deep attribution'
do { $choice = Read-Host 'Enter 1 through 36' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '31' { $Experiment = 'v36-hitch-combined'; $V36PerformanceProfile = 'true'; $V36Attribution = $true }
    '32' { $Experiment = 'v36-full-diagnostic'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0'; $V36Attribution = $true }
}''',
    '''    '31' { $Experiment = 'v36-hitch-combined'; $V36PerformanceProfile = 'true'; $V36Attribution = $true }
    '32' { $Experiment = 'v36-full-diagnostic'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0'; $V36Attribution = $true }
    '33' { $Experiment = 'v37-normal-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true' }
    '34' { $Experiment = 'v37-active-event-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37ActiveEventFastPath = 'true' }
    '35' { $Experiment = 'v37-keyframe-preload-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37CompanionKeyframePreload = 'true'; $V36Attribution = $true }
    '36' { $Experiment = 'v37-hitch-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V36Attribution = $true }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v36_async_gpu_profiler=$V36AsyncGpuProfiler",
    "v36_far_caster_minimum_pixels=$V36FarCasterMinimumPixels",
    "v36_deep_attribution=$V36Attribution",
    "shadow_distance=$ShadowDistance",''',
    '''    "v36_async_gpu_profiler=$V36AsyncGpuProfiler",
    "v36_far_caster_minimum_pixels=$V36FarCasterMinimumPixels",
    "v36_deep_attribution=$V36Attribution",
    "v37_active_event_fast_path=$V37ActiveEventFastPath",
    "v37_companion_keyframe_preload=$V37CompanionKeyframePreload",
    "v37_relaxed_resource_sweep=$V37RelaxedResourceSweep",
    "v37_resource_sweep_seconds=$V37ResourceSweepSeconds",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.6 async gpu profiler' $V36AsyncGpuProfiler
    Set-IniValue $SettingsPath 'V3' 'v3.6 far caster minimum pixels' $V36FarCasterMinimumPixels
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.6 async gpu profiler' $V36AsyncGpuProfiler
    Set-IniValue $SettingsPath 'V3' 'v3.6 far caster minimum pixels' $V36FarCasterMinimumPixels
    Set-IniValue $SettingsPath 'V3' 'v3.7 active event fast path' $V37ActiveEventFastPath
    Set-IniValue $SettingsPath 'V3' 'v3.7 companion keyframe preload' $V37CompanionKeyframePreload
    Set-IniValue $SettingsPath 'V3' 'v3.7 relaxed resource cache sweep' $V37RelaxedResourceSweep
    Set-IniValue $SettingsPath 'V3' 'v3.7 resource cache sweep seconds' $V37ResourceSweepSeconds
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.7 loaded-handler, companion-keyframe, resource-sweep, and launcher patch completed successfully.")
