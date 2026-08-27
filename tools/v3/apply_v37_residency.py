import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.7 residency match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.7 residency patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Adapter-aware pressure classification.
#
# DXGI is process-local WDDM usage/budget. NVML is adapter-wide. Earlier V3
# captures showed a material gap between them on an 8-GB hybrid-laptop setup, so
# process-local pressure alone can stay comfortable while the physical adapter
# is nearly full. This policy never evicts live GL objects. It only exposes a
# conservative pressure signal used below to reduce speculative preload work.
# -----------------------------------------------------------------------------
replace_exact(
    "components/debug/v3gpumemory.hpp",
    '''    inline bool hardPressure()
    {
        return pressureState() == PressureState::Hard;
    }

    inline const char* pressureName(PressureState state)''',
    '''    inline bool hardPressure()
    {
        return pressureState() == PressureState::Hard;
    }

    inline bool softPressure()
    {
        const PressureState state = pressureState();
        return state == PressureState::Soft || state == PressureState::Hard;
    }

    inline const char* pressureName(PressureState state)''',
)

replace_exact(
    "components/debug/v3gpumemory.hpp",
    '''        if (dxgiAvailable)
        {
            // Respect both the user's configured 8-GB-class limits and WDDM's actual
            // process budget. This preserves headroom when Windows temporarily grants
            // less than the adapter's physical capacity.
            softBytes = std::min(configuredSoftBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.90));
            hardBytes = std::min(configuredHardBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.97));
            if (hardBytes <= softBytes)
                hardBytes
                    = std::min<std::uint64_t>(sample.mBudgetBytes, softBytes + std::uint64_t{ 128 } * MiB);

            state = PressureState::Comfortable;
            if (sample.mUsageBytes >= hardBytes)
                state = PressureState::Hard;
            else if (sample.mUsageBytes >= softBytes)
                state = PressureState::Soft;
        }
        sPressureState.store(static_cast<int>(state), std::memory_order_relaxed);''',
    '''        if (dxgiAvailable)
        {
            // Respect both the user's configured 8-GB-class limits and WDDM's actual
            // process budget. This preserves headroom when Windows temporarily grants
            // less than the adapter's physical capacity.
            softBytes = std::min(configuredSoftBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.90));
            hardBytes = std::min(configuredHardBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.97));
            if (hardBytes <= softBytes)
                hardBytes
                    = std::min<std::uint64_t>(sample.mBudgetBytes, softBytes + std::uint64_t{ 128 } * MiB);

            state = PressureState::Comfortable;
            if (sample.mUsageBytes >= hardBytes)
                state = PressureState::Hard;
            else if (sample.mUsageBytes >= softBytes)
                state = PressureState::Soft;
        }

        if (nvmlAvailable && adapterSample.mTotalBytes != 0)
        {
            // Adapter-wide guard for shared pressure that DXGI's process-local
            // accounting cannot see. Because V3.7 uses this only for admission
            // control (never destructive eviction), it is intentionally early.
            const double adapterUsedRatio = static_cast<double>(adapterSample.mUsedBytes)
                / static_cast<double>(adapterSample.mTotalBytes);
            constexpr std::uint64_t AdapterSoftFreeBytes = std::uint64_t{ 900 } * MiB;
            constexpr std::uint64_t AdapterHardFreeBytes = std::uint64_t{ 450 } * MiB;

            PressureState adapterState = PressureState::Comfortable;
            if (adapterUsedRatio >= 0.94 || adapterSample.mFreeBytes <= AdapterHardFreeBytes)
                adapterState = PressureState::Hard;
            else if (adapterUsedRatio >= 0.88 || adapterSample.mFreeBytes <= AdapterSoftFreeBytes)
                adapterState = PressureState::Soft;

            if (state == PressureState::Unavailable || static_cast<int>(adapterState) > static_cast<int>(state))
                state = adapterState;
        }

        sPressureState.store(static_cast<int>(state), std::memory_order_relaxed);''',
)


# -----------------------------------------------------------------------------
# Predictive outer-ring preload admission.
#
# Required/current-cell activation is untouched. Already-preloaded candidates
# are still refreshed. Under soft pressure, admit at most one NEW speculative
# outer-ring cell per frame; under hard pressure, admit none until pressure
# falls. Configured V3.3 budgets keep their original behavior otherwise.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v32rendererprofiling.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v32rendererprofiling.hpp>
#include <components/debug/v3gpumemory.hpp>''',
)

replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''        unsigned attemptedNew = 0;
        unsigned refreshed = 0;
        unsigned deferred = 0;
        const unsigned budget = static_cast<unsigned>(mV33SpeculativePreloadBudget);
        for (const Candidate& candidate : candidates)
        {
            if (mPreloader->isPreloaded(*candidate.mCell))
            {
                preloadCell(*candidate.mCell);
                ++refreshed;
            }
            else if (budget == 0 || attemptedNew < budget)
            {
                preloadCell(*candidate.mCell);
                ++attemptedNew;
            }
            else
                ++deferred;
        }

        if (budget != 0 && Debug::V3Diagnostics::streamingWriter().enabled())
        {
            std::ostringstream detail;
            detail << "attempted_new=" << attemptedNew << ";refreshed=" << refreshed << ";deferred=" << deferred;
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ",budget,v33_speculative_preload," << Debug::V3Diagnostics::csvQuote(detail.str()) << ','
                << std::fixed << std::setprecision(3) << Debug::V3HitchTelemetry::lastFrameWallMs() << ','
                << budget << ',' << candidates.size();
            Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
        }''',
    '''        unsigned attemptedNew = 0;
        unsigned refreshed = 0;
        unsigned deferred = 0;
        const unsigned configuredBudget = static_cast<unsigned>(mV33SpeculativePreloadBudget);
        unsigned effectiveBudget = configuredBudget;
        bool blockNewPreloads = false;
        const bool v37PressureManagement = static_cast<bool>(Settings::cells().mV32GpuMemoryManagement);
        const Debug::V3GpuMemory::PressureState v37Pressure = v37PressureManagement
            ? Debug::V3GpuMemory::pressureState()
            : Debug::V3GpuMemory::PressureState::Unavailable;
        if (v37Pressure == Debug::V3GpuMemory::PressureState::Hard)
            blockNewPreloads = true;
        else if (v37Pressure == Debug::V3GpuMemory::PressureState::Soft
            && (effectiveBudget == 0 || effectiveBudget > 1))
            effectiveBudget = 1;

        for (const Candidate& candidate : candidates)
        {
            if (mPreloader->isPreloaded(*candidate.mCell))
            {
                preloadCell(*candidate.mCell);
                ++refreshed;
            }
            else if (!blockNewPreloads && (effectiveBudget == 0 || attemptedNew < effectiveBudget))
            {
                preloadCell(*candidate.mCell);
                ++attemptedNew;
            }
            else
                ++deferred;
        }

        if ((configuredBudget != 0 || (v37PressureManagement
                && v37Pressure != Debug::V3GpuMemory::PressureState::Unavailable))
            && Debug::V3Diagnostics::streamingWriter().enabled())
        {
            std::ostringstream detail;
            detail << "attempted_new=" << attemptedNew << ";refreshed=" << refreshed << ";deferred=" << deferred
                   << ";configured_budget=" << configuredBudget << ";effective_budget=" << effectiveBudget
                   << ";block_new=" << (blockNewPreloads ? 1 : 0)
                   << ";pressure=" << Debug::V3GpuMemory::pressureName(v37Pressure);
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ",budget,v37_pressure_aware_preload," << Debug::V3Diagnostics::csvQuote(detail.str()) << ','
                << std::fixed << std::setprecision(3) << Debug::V3HitchTelemetry::lastFrameWallMs() << ','
                << effectiveBudget << ',' << candidates.size();
            Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
        }''',
)


# -----------------------------------------------------------------------------
# If adapter pressure is already soft/hard, don't also stretch the maintenance
# sweep cadence. We simply return to upstream's one-second sweep. Expiry delays
# remain unchanged, and no live/cache GL object is explicitly released.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>''',
    '''#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/debug/v3gpumemory.hpp>
#include <components/settings/values.hpp>''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        const double v37ResourceSweepSeconds
            = static_cast<bool>(Settings::cells().mV37RelaxedResourceSweep)
            ? static_cast<double>(Settings::cells().mV37ResourceSweepSeconds)
            : 1.0;''',
    '''        const bool v37AdapterPressure = static_cast<bool>(Settings::cells().mV32GpuMemoryManagement)
            && Debug::V3GpuMemory::softPressure();
        const double v37ResourceSweepSeconds
            = static_cast<bool>(Settings::cells().mV37RelaxedResourceSweep) && !v37AdapterPressure
            ? static_cast<double>(Settings::cells().mV37ResourceSweepSeconds)
            : 1.0;''',
)


# -----------------------------------------------------------------------------
# Launcher: enable non-destructive adapter pressure management in the full V3.7
# candidates and expose one isolated comparison against V3.6 profile behavior.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V37RelaxedResourceSweep = 'false'
$V37ResourceSweepSeconds = '5.0'
$RendererProfiling''',
    '''$V37RelaxedResourceSweep = 'false'
$V37ResourceSweepSeconds = '5.0'
$V37GpuMemoryManagement = 'false'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 36 = V3.7 hitch combined + deep attribution'
do { $choice = Read-Host 'Enter 1 through 36' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36'))''',
    '''Write-Host ' 36 = V3.7 hitch combined + deep attribution'
Write-Host ' 37 = V3.7 adapter-aware speculative preload admission isolated'
do { $choice = Read-Host 'Enter 1 through 37' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '33' { $Experiment = 'v37-normal-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true' }
    '34' { $Experiment = 'v37-active-event-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37ActiveEventFastPath = 'true' }
    '35' { $Experiment = 'v37-keyframe-preload-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37CompanionKeyframePreload = 'true'; $V36Attribution = $true }
    '36' { $Experiment = 'v37-hitch-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V36Attribution = $true }
}''',
    '''    '33' { $Experiment = 'v37-normal-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true' }
    '34' { $Experiment = 'v37-active-event-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37ActiveEventFastPath = 'true' }
    '35' { $Experiment = 'v37-keyframe-preload-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37CompanionKeyframePreload = 'true'; $V36Attribution = $true }
    '36' { $Experiment = 'v37-hitch-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V36Attribution = $true }
    '37' { $Experiment = 'v37-vram-admission-isolated'; $V36PerformanceProfile = 'true'; $V37GpuMemoryManagement = 'true' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v37_resource_sweep_seconds=$V37ResourceSweepSeconds",
    "shadow_distance=$ShadowDistance",''',
    '''    "v37_resource_sweep_seconds=$V37ResourceSweepSeconds",
    "v37_gpu_memory_management=$V37GpuMemoryManagement",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' 'false' ''',
    '''    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' $V37GpuMemoryManagement ''',
)

print("V3.7 adapter-wide pressure and non-destructive residency admission patch completed successfully.")
