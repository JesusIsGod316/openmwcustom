import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.8 traversal/GPU match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.8 traversal/GPU patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Runtime profiles.
#
# World batching modes:
#   0 = V3.7/upstream paging heuristic
#   1 = conservative: increase distant merge pressure only
#   2 = moderate: stronger distant merging + repeated-template preference
#   3 = aggressive: merge every eligible distant static template and repeated
#       active-grid templates, plus vertex-cache/order optimization.
#
# GPU residency modes:
#   0 = V3.7 admission/sweep behavior only
#   1 = conservative stale render-cache reclamation under hard pressure
#   2 = moderate pressure reclamation
#   3 = aggressive pressure reclamation
#
# The residency path drops only CACHE-ONLY scene/paged objects by using the
# existing ObjectCache expiry semantics. Live scene nodes remain externally
# referenced and therefore cannot be removed. CPU-side NIF/image caches are not
# shortened here, preserving the project's host-RAM-heavy design.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<bool> mV37StabilizeFarCascade{ mIndex, "V3", "v3.7 stabilize far shadow cascade" };''',
    '''        SettingValue<bool> mV37StabilizeFarCascade{ mIndex, "V3", "v3.7 stabilize far shadow cascade" };

        SettingValue<int> mV38WorldBatchingMode{ mIndex, "V3", "v3.8 world batching mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<float> mV38WorldBatchingMergeMultiplier{ mIndex, "V3",
            "v3.8 world batching merge multiplier", makeClampSanitizerFloat(1, 8) };
        SettingValue<int> mV38WorldBatchingMinInstances{ mIndex, "V3",
            "v3.8 world batching min instances", makeClampSanitizerInt(2, 64) };
        SettingValue<int> mV38GpuResidencyMode{ mIndex, "V3", "v3.8 gpu residency mode",
            makeClampSanitizerInt(0, 3) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# Snap only the far orthographic cascade to its actual texture texel grid.
# This is a visual-stability experiment, not whole-map reuse, and is off until A/B tested.
v3.7 stabilize far shadow cascade = false

[Cells]''',
    '''# Snap only the far orthographic cascade to its actual texture texel grid.
# This is a visual-stability experiment, not whole-map reuse, and is off until A/B tested.
v3.7 stabilize far shadow cascade = false

# V3.8 traversal-smoothness/GPU-efficiency controls.
# world batching mode: 0=V3.7 heuristic, 1=conservative, 2=moderate, 3=aggressive.
# The merge multiplier is applied on top of OpenMW's existing object-paging merge factor.
# Repeated-template forcing starts at mode 2 and obeys the min-instance threshold.
v3.8 world batching mode = 0
v3.8 world batching merge multiplier = 1.5
v3.8 world batching min instances = 2

# GPU residency mode: 0=off, 1=conservative, 2=moderate, 3=aggressive.
# Reclamation only shortens the lifetime of stale cache-only rendered scene/paged
# objects while adapter pressure is elevated. It does not shorten NIF/image source
# caches and cannot evict live scene nodes.
v3.8 gpu residency mode = 0

[Cells]''',
)

# -----------------------------------------------------------------------------
# Aggressive world batching: extend OpenMW's EXISTING worker-side paging
# optimizer. This avoids a second conflicting instancing/merge implementation.
# Compatibility filtering, update-traversal exclusion, LOD selection, StateSet
# compatibility, alpha handling and refnum markers remain the existing OpenMW
# paths. Modes only change how strongly eligible templates are admitted to the
# already-existing mergeGroup and which safe mesh-order optimization passes run.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const float mergeCost = analyzeResult.mNumVerts * size;
            const float mergeBenefit = analyzeVisitor.getMergeBenefit(analyzeResult) * mMergeFactor;
            const bool merge = mergeBenefit > mergeCost;

            const float factor2''',
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const unsigned int v38InstanceCount = static_cast<unsigned int>(pair.second.mInstances.size());
            float v38MergeMultiplier = 1.f;
            if (v38BatchingMode > 0)
            {
                const float configuredMultiplier
                    = static_cast<float>(Settings::cells().mV38WorldBatchingMergeMultiplier);
                // Distant chunks are the highest-payoff population and are immutable.
                // Keep active-grid pressure lower because those chunks also carry refnum
                // interaction bookkeeping.
                if (!activeGrid)
                    v38MergeMultiplier = configuredMultiplier
                        * (v38BatchingMode == 1 ? 1.f : (v38BatchingMode == 2 ? 1.75f : 3.f));
                else if (v38BatchingMode >= 2)
                    v38MergeMultiplier = v38BatchingMode == 2 ? 1.15f : 1.5f;
            }

            const float mergeCost = analyzeResult.mNumVerts * size;
            const float mergeBenefit
                = analyzeVisitor.getMergeBenefit(analyzeResult) * mMergeFactor * v38MergeMultiplier;
            const bool v38RepeatedCandidate = v38BatchingMode >= 2
                && v38InstanceCount >= static_cast<unsigned int>(Settings::cells().mV38WorldBatchingMinInstances);
            const bool v38ForceMerge = (v38BatchingMode >= 3 && !activeGrid)
                || (v38RepeatedCandidate && (!activeGrid || v38BatchingMode >= 3));
            const bool merge = mergeBenefit > mergeCost || v38ForceMerge;

            const float factor2''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const unsigned int options = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;

            optimizer.optimize(mergeGroup, options);''',
    '''            unsigned int options = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;

            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            // These passes operate on the worker-built merged geometry, preserving
            // rendered content while improving post-transform cache locality. Keep the
            // more expensive access-order pass for the aggressive profile.
            if (v38BatchingMode >= 2)
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            if (v38BatchingMode >= 3)
                options |= SceneUtil::Optimizer::VERTEX_PRETRANSFORM;

            optimizer.optimize(mergeGroup, options);''',
)

# -----------------------------------------------------------------------------
# Pressure-only render-cache reclamation.
# Add a public expiry override that reuses ObjectCache's proven semantics:
# externally referenced/live objects refresh their last-use timestamp and cannot
# be erased. This gives us actual stale render-resource reclamation without
# calling releaseGLObjects on potentially shared live textures.
# -----------------------------------------------------------------------------
replace_exact(
    "components/resource/resourcemanager.hpp",
    '''        /// Clear cache entries that have not been referenced for longer than expiryDelay.
        void updateCache(double referenceTime) override { mCache->update(referenceTime, mExpiryDelay); }

        /// Clear all cache entries.''',
    '''        /// Clear cache entries that have not been referenced for longer than expiryDelay.
        void updateCache(double referenceTime) override { mCache->update(referenceTime, mExpiryDelay); }

        /// Pressure-only override used by V3.8. This deliberately reuses the
        /// normal object-cache reference-count safety: live/external objects are
        /// refreshed and cannot be erased, while stale cache-only objects may be
        /// released earlier than the normal long host-residency expiry.
        void updateCacheWithExpiry(double referenceTime, double expiryDelay)
        {
            mCache->update(referenceTime, expiryDelay);
        }

        /// Clear all cache entries.''',
)

# V3.8 needs the adapter pressure sampler even when legacy V3.2 management is
# disabled but the new reclamation mode is active.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        if (Settings::cells().mV32GpuMemoryTelemetry || Settings::cells().mV32GpuMemoryManagement)
        {
            Debug::V3GpuMemory::sampleIfDue(static_cast<bool>(Settings::cells().mV32GpuMemoryTelemetry),
                static_cast<bool>(Settings::cells().mV32GpuMemoryManagement),
                static_cast<int>(Settings::cells().mV32GpuSoftBudgetMb),
                static_cast<int>(Settings::cells().mV32GpuHardBudgetMb));
        }
        reportStats();''',
    '''        const int v38ResidencyMode = static_cast<int>(Settings::cells().mV38GpuResidencyMode);
        const bool v38ResidencyEnabled = v38ResidencyMode > 0;
        if (Settings::cells().mV32GpuMemoryTelemetry || Settings::cells().mV32GpuMemoryManagement
            || v38ResidencyEnabled)
        {
            Debug::V3GpuMemory::sampleIfDue(static_cast<bool>(Settings::cells().mV32GpuMemoryTelemetry),
                static_cast<bool>(Settings::cells().mV32GpuMemoryManagement) || v38ResidencyEnabled,
                static_cast<int>(Settings::cells().mV32GpuSoftBudgetMb),
                static_cast<int>(Settings::cells().mV32GpuHardBudgetMb));
        }

        if (v38ResidencyEnabled && mViewer->getFrameStamp())
        {
            const Debug::V3GpuMemory::PressureState pressure = Debug::V3GpuMemory::pressureState();
            if (pressure == Debug::V3GpuMemory::PressureState::Soft
                || pressure == Debug::V3GpuMemory::PressureState::Hard)
            {
                const double now = mViewer->getFrameStamp()->getReferenceTime();
                static double sV38LastResidencyTrim = -1000.0;

                double interval = 2.0;
                double sceneAge = 90.0;
                double pagingAge = 120.0;
                if (v38ResidencyMode == 1)
                {
                    // Conservative mode waits for hard pressure and only trims
                    // very stale render templates.
                    if (pressure == Debug::V3GpuMemory::PressureState::Soft)
                        interval = -1.0;
                    sceneAge = 90.0;
                    pagingAge = 180.0;
                }
                else if (v38ResidencyMode == 2)
                {
                    interval = pressure == Debug::V3GpuMemory::PressureState::Hard ? 0.75 : 1.5;
                    sceneAge = pressure == Debug::V3GpuMemory::PressureState::Hard ? 20.0 : 45.0;
                    pagingAge = pressure == Debug::V3GpuMemory::PressureState::Hard ? 30.0 : 60.0;
                }
                else
                {
                    interval = pressure == Debug::V3GpuMemory::PressureState::Hard ? 0.35 : 0.75;
                    sceneAge = pressure == Debug::V3GpuMemory::PressureState::Hard ? 5.0 : 15.0;
                    pagingAge = pressure == Debug::V3GpuMemory::PressureState::Hard ? 8.0 : 20.0;
                }

                if (interval > 0.0 && now - sV38LastResidencyTrim >= interval)
                {
                    // Scene templates are the primary GPU-backed cache. Source
                    // NIF/image/keyframe managers retain their long Overdrive
                    // expiry, so re-materialization remains RAM-heavy rather than
                    // disk-heavy if an evicted render template is needed again.
                    mResourceSystem->getSceneManager()->updateCacheWithExpiry(now, sceneAge);

                    // Paged chunks are already immutable prepared render graphs.
                    // Mode 1 leaves their long cache untouched; moderate/aggressive
                    // can release stale cache-only chunks under pressure.
                    if (v38ResidencyMode >= 2 && mObjectPaging)
                        mObjectPaging->updateCacheWithExpiry(now, pagingAge);
                    if (v38ResidencyMode >= 3 && mGroundcover)
                        mGroundcover->updateCacheWithExpiry(now, pagingAge);

                    sV38LastResidencyTrim = now;
                }
            }
        }

        reportStats();''',
)

# -----------------------------------------------------------------------------
# Unified launcher profiles. Choices 39-48 are runtime modes in ONE executable.
# The new normal candidate deliberately turns off unvalidated V3.7 keyframe
# preload and far stabilization while retaining the proven V3.6/V3.7 stack.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V37StabilizeFarCascade = 'false'
$RendererProfiling''',
    '''$V37StabilizeFarCascade = 'false'
$V38WorldBatchingMode = '0'
$V38WorldBatchingMergeMultiplier = '1.5'
$V38WorldBatchingMinInstances = '2'
$V38GpuResidencyMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 38 = V3.7 far-cascade texel stabilization at 6144 shadow distance'
do { $choice = Read-Host 'Enter 1 through 38' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38'))''',
    '''Write-Host ' 38 = V3.7 far-cascade texel stabilization at 6144 shadow distance'
Write-Host ' 39 = V3.8 clean traversal baseline (proven stack, new mechanisms off)'
Write-Host ' 40 = V3.8 world batching conservative'
Write-Host ' 41 = V3.8 world batching moderate'
Write-Host ' 42 = V3.8 world batching aggressive'
Write-Host ' 43 = V3.8 GPU residency conservative'
Write-Host ' 44 = V3.8 GPU residency moderate'
Write-Host ' 45 = V3.8 GPU residency aggressive'
Write-Host ' 46 = V3.8 traversal combined conservative'
Write-Host ' 47 = V3.8 traversal combined moderate'
Write-Host ' 48 = V3.8 traversal combined aggressive'
do { $choice = Read-Host 'Enter 1 through 48' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '38' { $Experiment = 'v37-far-stabilization-6144'; $V36PerformanceProfile = 'true'; $V37StabilizeFarCascade = 'true'; $ShadowDistance = '6144' }
}''',
    '''    '38' { $Experiment = 'v37-far-stabilization-6144'; $V36PerformanceProfile = 'true'; $V37StabilizeFarCascade = 'true'; $ShadowDistance = '6144' }
    '39' { $Experiment = 'v38-traversal-baseline'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true' }
    '40' { $Experiment = 'v38-batching-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1' }
    '41' { $Experiment = 'v38-batching-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2' }
    '42' { $Experiment = 'v38-batching-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3' }
    '43' { $Experiment = 'v38-residency-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '1' }
    '44' { $Experiment = 'v38-residency-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '2' }
    '45' { $Experiment = 'v38-residency-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '3' }
    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v37_stabilize_far_cascade=$V37StabilizeFarCascade",
    "shadow_distance=$ShadowDistance",''',
    '''    "v37_stabilize_far_cascade=$V37StabilizeFarCascade",
    "v38_world_batching_mode=$V38WorldBatchingMode",
    "v38_world_batching_merge_multiplier=$V38WorldBatchingMergeMultiplier",
    "v38_world_batching_min_instances=$V38WorldBatchingMinInstances",
    "v38_gpu_residency_mode=$V38GpuResidencyMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.7 stabilize far shadow cascade' $V37StabilizeFarCascade
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.7 stabilize far shadow cascade' $V37StabilizeFarCascade
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching mode' $V38WorldBatchingMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching merge multiplier' $V38WorldBatchingMergeMultiplier
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching min instances' $V38WorldBatchingMinInstances
    Set-IniValue $SettingsPath 'V3' 'v3.8 gpu residency mode' $V38GpuResidencyMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.8 traversal smoothness: aggressive world batching and pressure residency reclamation patched successfully.")
