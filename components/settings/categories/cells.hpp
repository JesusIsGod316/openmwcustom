#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CELLS_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CELLS_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct CellsCategory : WithIndex
    {
        using WithIndex::WithIndex;

        // V3.6 keeps profile controls in one category object while preserving the user-facing [V3] section.
        SettingValue<bool> mV36PerformanceProfile{ mIndex, "V3", "v3.6 performance profile" };
        SettingValue<bool> mV36DisableRamOverdrive{ mIndex, "V3", "v3.6 disable ram overdrive" };
        SettingValue<bool> mV36DisableLuaFastPath{ mIndex, "V3", "v3.6 disable lua fast path" };
        SettingValue<bool> mV36DisableCoarseChunkOcclusion{
            mIndex, "V3", "v3.6 disable coarse chunk occlusion" };
        SettingValue<bool> mV36AsyncGpuProfiler{ mIndex, "V3", "v3.6 async gpu profiler" };
        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "V3", "v3.6 far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };
        // V3.19 stable gaming: promoted focus temporal-coherence cadence.
        SettingValue<int> mV319FocusCadence{
            mIndex, "V3", "v3.19 focus cadence", makeClampSanitizerInt(1, 3) };
        // V3.20: optionally force a focus refresh when the main camera contract changes.
        // Disabled by default so the promoted fixed-cadence P0 path remains exact.
        SettingValue<bool> mV320AdaptiveFocusCadence{
            mIndex, "V3", "v3.20 adaptive focus cadence" };
        // V3.21 CP1 downstream completed-work admission. Mode 0 preserves the
        // V3.20 behavior; mode 1 enables the fixed governor.
        SettingValue<int> mV321CompletionGovernorMode{
            mIndex, "V3", "v3.21 completion governor mode", makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV321CompileObjectsPerFrame{
            mIndex, "V3", "v3.21 compile objects per frame", makeClampSanitizerInt(1, 20) };
        SettingValue<int> mV321MergeSetsPerFrame{
            mIndex, "V3", "v3.21 merge sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321MaxDeferredFrames{
            mIndex, "V3", "v3.21 max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321ForcedMergeSets{
            mIndex, "V3", "v3.21 forced merge sets", makeClampSanitizerInt(0, 8) };
        SettingValue<float> mV321CompileMinimumMilliseconds{
            mIndex, "V3", "v3.21 compile minimum milliseconds", makeClampSanitizerFloat(0.1, 4.0) };
        SettingValue<float> mV321CompileConservativeRatio{
            mIndex, "V3", "v3.21 compile conservative ratio", makeClampSanitizerFloat(0.1, 1.0) };
        // MODE 127 adaptive merge-admission controls. Adaptation uses only the
        // previously completed frame and a bounded EMA/debt state.
        SettingValue<float> mV321AdaptiveTargetMilliseconds{
            mIndex, "V3", "v3.21 adaptive target milliseconds", makeClampSanitizerFloat(8.0, 50.0) };
        SettingValue<float> mV321AdaptiveFrameEmaAlpha{
            mIndex, "V3", "v3.21 adaptive frame ema alpha", makeClampSanitizerFloat(0.05, 1.0) };
        SettingValue<int> mV321AdaptiveMergeMin{
            mIndex, "V3", "v3.21 adaptive merge minimum", makeClampSanitizerInt(1, 8) };
        SettingValue<int> mV321AdaptiveMergeMax{
            mIndex, "V3", "v3.21 adaptive merge maximum", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321AdaptiveDebtCap{
            mIndex, "V3", "v3.21 adaptive debt cap", makeClampSanitizerInt(0, 32) };
        SettingValue<int> mV321AdaptiveDebtRepayPerFrame{
            mIndex, "V3", "v3.21 adaptive debt repay per frame", makeClampSanitizerInt(0, 4) };
        // V3.21 CP2 class-aware completion fairness/dephasing. Default off.
        SettingValue<int> mV321CP2FairnessMode{
            mIndex, "V3", "v3.21 CP2 fairness mode", makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV321CP2ServiceSetsPerFrame{
            mIndex, "V3", "v3.21 CP2 service sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2ClassBurstSetsPerFrame{
            mIndex, "V3", "v3.21 CP2 class burst sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2MaxDeferredFrames{
            mIndex, "V3", "v3.21 CP2 max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321CP2ForcedSets{
            mIndex, "V3", "v3.21 CP2 forced sets", makeClampSanitizerInt(0, 8) };
        SettingValue<int> mV321CP2DeficitCap{
            mIndex, "V3", "v3.21 CP2 deficit cap", makeClampSanitizerInt(1, 32) };
        SettingValue<bool> mV37DisableFarCasterPruning{
            mIndex, "V3", "v3.7 disable far caster pruning" };
        SettingValue<bool> mV37ActiveEventFastPath{ mIndex, "V3", "v3.7 active event fast path" };
        SettingValue<bool> mV37CompanionKeyframePreload{
            mIndex, "V3", "v3.7 companion keyframe preload" };
        SettingValue<bool> mV37RelaxedResourceSweep{
            mIndex, "V3", "v3.7 relaxed resource cache sweep" };
        SettingValue<float> mV37ResourceSweepSeconds{ mIndex, "V3", "v3.7 resource cache sweep seconds",
            makeClampSanitizerFloat(0.5, 60) };
        SettingValue<bool> mV37StabilizeFarCascade{ mIndex, "V3", "v3.7 stabilize far shadow cascade" };

        SettingValue<int> mV38WorldBatchingMode{ mIndex, "V3", "v3.8 world batching mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<float> mV38WorldBatchingMergeMultiplier{ mIndex, "V3",
            "v3.8 world batching merge multiplier", makeClampSanitizerFloat(1, 8) };
        SettingValue<int> mV38WorldBatchingMinInstances{ mIndex, "V3",
            "v3.8 world batching min instances", makeClampSanitizerInt(2, 64) };
        SettingValue<int> mV38GpuResidencyMode{ mIndex, "V3", "v3.8 gpu residency mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38FarShadowMode{ mIndex, "V3", "v3.8 far shadow mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38CompilePacingMode{ mIndex, "V3", "v3.8 compile pacing mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<int> mV39FrontloadMode{ mIndex, "V3", "v3.9 frontload mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39BatchOptimizerMode{ mIndex, "V3", "v3.9 batch optimizer mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39ProactiveResidencyMode{ mIndex, "V3", "v3.9 proactive residency mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<bool> mV310FreshInitialObjectPaging{ mIndex, "V3",
            "v3.10 fresh initial object paging" };
        SettingValue<bool> mV310PreloadPostTransform{ mIndex, "V3", "v3.10 preload post-transform" };

        SettingValue<int> mV311ActiveGridPrepareMode{ mIndex, "V3", "v3.11 active grid prepare mode",
            makeClampSanitizerInt(0, 2) };

        SettingValue<int> mV312PredictorMode{ mIndex, "V3", "v3.12 predictor mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<float> mV312PredictorLeadSeconds{ mIndex, "V3", "v3.12 predictor lead seconds",
            makeClampSanitizerFloat(0.25, 8.0) };
        SettingValue<bool> mV312LuaPrecompile{ mIndex, "V3", "v3.12 lua precompile" };
        SettingValue<int> mV312SpatialBatchMode{ mIndex, "V3", "v3.12 spatial batch mode",
            makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV313ChunkQualityMode{ mIndex, "V3", "v3.13 chunk quality mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV314LuaDependencyPrecompileMode{ mIndex, "V3", "v3.14 lua dependency precompile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314LuaPackagePrototypeReuse{ mIndex, "V3", "v3.14 lua package prototype reuse" };
        SettingValue<int> mV314GroundcoverCompileMode{ mIndex, "V3", "v3.14 groundcover compile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314PostfxCompileWarmup{ mIndex, "V3", "v3.14 postfx compile warmup" };

        SettingValue<bool> mV315PremergeStateCanonicalization{ mIndex, "V3",
            "v3.15 premerge state canonicalization" };
        SettingValue<int> mV315PacketizedPremergeMode{ mIndex, "V3", "v3.15 packetized premerge mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV316IdleResourceSweep{ mIndex, "V3", "v3.16 idle resource sweep" };
        SettingValue<bool> mV316SfxMetadataFrontload{ mIndex, "V3", "v3.16 sfx metadata frontload" };

        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme", "overdrive" }) };
        SettingValue<std::string> mRamCacheOverdrivePreload{ mIndex, "Cells", "ram cache overdrive preload",
            makeEnumSanitizerString({ "balanced", "aggressive", "maximum" }) };
        SettingValue<std::string> mV3StreamingScheduler{ mIndex, "Cells", "v3 streaming scheduler",
            makeEnumSanitizerString({ "off", "adaptive", "adaptive-v2" }) };
        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mV32StreamingMaxDefers{ mIndex, "Cells", "v3.2 streaming max defers",
            makeClampSanitizerInt(0, 16) };
        SettingValue<bool> mV3PreparedInstanceCache{ mIndex, "Cells", "v3 prepared instance cache" };
        SettingValue<int> mV3PreparedInstanceCacheMax{ mIndex, "Cells", "v3 prepared instance cache max",
            makeClampSanitizerInt(256, 65536) };
        SettingValue<bool> mV32ExteriorHibernation{ mIndex, "Cells", "v3.2 exterior hibernation" };
        SettingValue<bool> mV32RendererInsertionProfiling{ mIndex, "Cells", "v3.2 renderer insertion profiling" };
        SettingValue<bool> mV32GpuMemoryTelemetry{ mIndex, "Cells", "v3.2 gpu memory telemetry" };
        SettingValue<bool> mV32GpuMemoryManagement{ mIndex, "Cells", "v3.2 gpu memory management" };
        SettingValue<int> mV32GpuSoftBudgetMb{ mIndex, "Cells", "v3.2 gpu soft budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV32GpuHardBudgetMb{ mIndex, "Cells", "v3.2 gpu hard budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV33SpeculativePreloadBudget{ mIndex, "Cells", "v3.3 speculative preload budget",
            makeClampSanitizerInt(0, 64) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };
        SettingValue<int> mPreloadNumThreads{ mIndex, "Cells", "preload num threads", makeMaxSanitizerInt(1) };
        SettingValue<bool> mPreloadExteriorGrid{ mIndex, "Cells", "preload exterior grid" };
        SettingValue<bool> mPreloadFastTravel{ mIndex, "Cells", "preload fast travel" };
        SettingValue<bool> mPreloadDoors{ mIndex, "Cells", "preload doors" };
        SettingValue<float> mPreloadDistance{ mIndex, "Cells", "preload distance", makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mPreloadInstances{ mIndex, "Cells", "preload instances" };
        SettingValue<int> mPreloadCellCacheMin{ mIndex, "Cells", "preload cell cache min", makeMaxSanitizerInt(1) };
        SettingValue<int> mPreloadCellCacheMax{ mIndex, "Cells", "preload cell cache max", makeMaxSanitizerInt(1) };
        SettingValue<float> mPreloadCellExpiryDelay{ mIndex, "Cells", "preload cell expiry delay",
            makeMaxSanitizerFloat(0) };
        SettingValue<float> mPredictionTime{ mIndex, "Cells", "prediction time", makeMaxSanitizerFloat(0) };
        SettingValue<float> mCacheExpiryDelay{ mIndex, "Cells", "cache expiry delay", makeMaxSanitizerFloat(0) };
        SettingValue<float> mTargetFramerate{ mIndex, "Cells", "target framerate", makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mPointersCacheSize{ mIndex, "Cells", "pointers cache size", makeClampSanitizerInt(40, 1000) };
    };
}

#endif
