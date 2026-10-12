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

        SettingValue<bool> mOptimizedMWOwnedCellCull{ mIndex, "Optimizations", "owned cell cull snapshots" };
        SettingValue<bool> mOptimizedMWUnchangedTransform{ mIndex, "Optimizations", "unchanged transform bounds" };
        SettingValue<bool> mOptimizedMWTemplateCoalescing{ mIndex, "Cells", "scene template coalescing" };
        SettingValue<bool> mOptimizedMWTemporalHistoryLookup{ mIndex, "Optimizations", "temporal history lookup" };
        SettingValue<bool> mOptimizedMWStaticVao{ mIndex, "Optimizations", "paged static vertex arrays" };

        // P2: cancellable, restart-indexed paging optimization and two-phase readiness.
        SettingValue<bool> mOpimizedMWPagingOptimizer{ mIndex, "Cells", "paging optimizer" };
        SettingValue<bool> mOpimizedMWPagingReadinessSplit{ mIndex, "Cells", "paging readiness split" };

        // OptimizedMW GL-P3: preserve primitive order/material semantics while compacting
        // adjacent indexed draws that differ only in index storage width.
        SettingValue<bool> mOptimizedMWSubmissionCompaction{
            mIndex, "Cells", "submission compaction" };
        SettingValue<bool> mOptimizedMWSemanticPremerge{
            mIndex, "Cells", "semantic premerge" };
        SettingValue<bool> mOptimizedMWDistantDisplayLists{
            mIndex, "Cells", "distant display lists" };
        SettingValue<bool> mOptimizedMWNormalizedStaticPackets{
            mIndex, "Cells", "normalized static packets" };
        SettingValue<bool> mOptimizedMWPrepareMaterialImages{
            mIndex, "Cells", "prepare material images" };
        SettingValue<bool> mLoadingNifMaintenance{
            mIndex, "Cells", "loading nif maintenance" };
        SettingValue<float> mUnusedImageCacheSeconds{
            mIndex, "Cells", "unused image cache seconds", makeClampSanitizerFloat(0, 3600) };
        SettingValue<float> mUnusedSceneCacheSeconds{
            mIndex, "Cells", "unused scene cache seconds", makeClampSanitizerFloat(0, 3600) };
        SettingValue<int> mOptimizedMWMaterialImageBudget{
            mIndex, "Cells", "material image preparation mib", makeClampSanitizerInt(16, 256) };
        SettingValue<bool> mOptimizedMWLodCacheHitAdmission{
            mIndex, "Cells", "lod cache hit admission" };
        SettingValue<bool> mOptimizedMWLodRetainSourceIndices{
            mIndex, "Cells", "lod retain source indices" };
        SettingValue<bool> mOptimizedMWOrdinaryOpaquePackets{
            mIndex, "Cells", "ordinary opaque packets" };
        SettingValue<bool> mOptimizedMWIndirectCommandReuse{
            mIndex, "Optimizations", "indirect command reuse" };
        SettingValue<bool> mOptimizedMWIndirectContextCapabilityReuse{
            mIndex, "Optimizations", "indirect context capability reuse" };
        SettingValue<bool> mOptimizedMWGpuVisibilityOpportunityFilter{
            mIndex, "Optimizations", "gpu visibility opportunity filter" };
        SettingValue<bool> mOptimizedMWIndirectProjectionProofReuse{
            mIndex, "Optimizations", "indirect projection proof reuse" };
        SettingValue<bool> mOptimizedMWIndirectEarlyAdmissionFallback{
            mIndex, "Optimizations", "indirect early admission fallback" };
        SettingValue<bool> mOptimizedMWPagingScoreValues{
            mIndex, "Cells", "paging score value reuse" };
        SettingValue<bool> mOptimizedMWPreloadPriorityPromotion{
            mIndex, "Cells", "preload priority promotion" };
        SettingValue<bool> mOptimizedMWActiveShadowBatching{ mIndex, "Shadows", "active batching" };
        SettingValue<bool> mOptimizedMWShadowStaticBatching{
            mIndex, "Shadows", "static batching" };
        SettingValue<bool> mOptimizedMWRenderHandoffAttribution{
            mIndex, "Optimizations", "render handoff attribution" };
        SettingValue<bool> mOptimizedMWParallelTemplatePrefetch{
            mIndex, "Cells", "parallel template prefetch" };
        SettingValue<int> mOptimizedMWParallelTemplatePrefetchWorkers{
            mIndex, "Cells", "parallel template prefetch workers",
            makeClampSanitizerInt(1, 2) };
        SettingValue<int> mOptimizedMWParallelTemplatePrefetchMinTemplates{
            mIndex, "Cells", "parallel template prefetch minimum templates",
            makeClampSanitizerInt(4, 512) };

        // OptimizedMW GL-P4: startup-selected replacement for OSG ICO scheduling.
        // Mode 0 preserves stock/current ICO behavior, 1 adds cost-aware admission,
        // and 2 adds rendering-handoff feedback plus bounded compile credit.
        SettingValue<int> mOptimizedMWCompileSchedulerMode{
            mIndex, "Cells", "compile scheduler mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<float> mOptimizedMWCompileSchedulerMaxBudgetMs{
            mIndex, "Cells", "compile scheduler max budget milliseconds",
            makeClampSanitizerFloat(0.1, 8.0) };
        SettingValue<float> mOptimizedMWCompileSchedulerCreditCapMs{
            mIndex, "Cells", "compile scheduler credit cap milliseconds",
            makeClampSanitizerFloat(0.1, 16.0) };
        SettingValue<float> mOptimizedMWCompileSchedulerHeadroomRatio{
            mIndex, "Cells", "compile scheduler headroom ratio",
            makeClampSanitizerFloat(0.05, 1.0) };
        SettingValue<float> mOptimizedMWCompileSchedulerHandoffThresholdMs{
            mIndex, "Cells", "compile scheduler handoff threshold milliseconds",
            makeClampSanitizerFloat(5.0, 100.0) };
        SettingValue<int> mOptimizedMWCompileSchedulerMaxQueueAgeFrames{
            mIndex, "Cells", "compile scheduler max queue age frames",
            makeClampSanitizerInt(1, 240) };
        SettingValue<float> mOptimizedMWCompileSchedulerDeleteBudgetMs{
            mIndex, "Cells", "compile scheduler delete budget milliseconds",
            makeClampSanitizerFloat(0.0, 4.0) };
        SettingValue<int> mOptimizedMWCompileSchedulerMaxObjectsPerFrame{
            mIndex, "Cells", "compile scheduler max objects per frame",
            makeClampSanitizerInt(1, 32) };
        SettingValue<float> mOptimizedMWCompileSchedulerDiagnosticThresholdMs{
            mIndex, "Cells", "compile scheduler diagnostic threshold milliseconds",
            makeClampSanitizerFloat(0.0, 20.0) };

        // P4R/P5: optional lane for individual GL calls whose observed/predicted
        // duration is too large for the ordinary frame budget.
        SettingValue<int> mOptimizedMWHeavyCompileLaneMode{
            mIndex, "Cells", "heavy compile lane mode",
            makeClampSanitizerInt(0, 1) };
        SettingValue<float> mOptimizedMWHeavyCompileThresholdMs{
            mIndex, "Cells", "heavy compile threshold milliseconds",
            makeClampSanitizerFloat(1.0, 50.0) };
        SettingValue<int> mOptimizedMWHeavyCompileMinSmoothFrames{
            mIndex, "Cells", "heavy compile minimum smooth frames",
            makeClampSanitizerInt(1, 600) };
        SettingValue<float> mOptimizedMWHeavyCompileMinHeadroomMs{
            mIndex, "Cells", "heavy compile minimum headroom milliseconds",
            makeClampSanitizerFloat(0.5, 16.0) };
        SettingValue<float> mOptimizedMWTerrainDrawablePriorMs{
            mIndex, "Cells", "terrain drawable prior milliseconds",
            makeClampSanitizerFloat(0.0, 50.0) };
        SettingValue<bool> mOptimizedMWTerrainPhasedCompile{
            mIndex, "Cells", "terrain phased compile" };

        // P6 stutter-first residency experiments. All remain default-off.
        SettingValue<int> mOptimizedMWResidencySchedulerMode{
            mIndex, "Cells", "residency scheduler mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mOptimizedMWTerrainImmutableVertexReuse{
            mIndex, "Cells", "terrain immutable vertex reuse" };
        // P6R: expose TerrainDrawable pass resources as native texture/program
        // compile operations instead of generic StateAttribute calls.
        SettingValue<bool> mOptimizedMWTerrainResourcePhases{
            mIndex, "Cells", "terrain resource phases" };
        // P6R: split the mutable terrain position/normal/color streams across
        // independent VBOs so GL upload work has scheduler interruption points.
        SettingValue<bool> mOptimizedMWTerrainSplitVertexBuffers{
            mIndex, "Cells", "terrain split vertex buffers" };

        // P8U1: first resource-repair slice, independently startup-switchable.
        SettingValue<bool> mOptimizedMWCanonicalTerrainTextures{
            mIndex, "Cells", "canonical terrain textures" };
        SettingValue<bool> mOptimizedMWCompositeSlicing{
            mIndex, "Cells", "composite slicing" };

        // P7 CPU preparation. These remain backend-neutral and default-off.
        SettingValue<bool> mOptimizedMWParallelActorBinding{
            mIndex, "Cells", "parallel actor binding" };
        SettingValue<bool> mOptimizedMWParallelTerrainCpuPrep{
            mIndex, "Cells", "parallel terrain cpu preparation" };

        // P8-HL1 ceiling probe. Startup-only OpenGL threading selection:
        // 0=OSG automatic/current behavior, 1=DrawThreadPerContext,
        // 2=CullDrawThreadPerContext, 3=SingleThreaded.
        SettingValue<int> mOptimizedMWOsgThreadingMode{
            mIndex, "Optimizations", "render threading mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<bool> mOpimizedMWSpeculativeBudget{ mIndex, "Cells", "speculative memory budget" };
        SettingValue<int> mOpimizedMWTransientBudgetMb{ mIndex, "Cells", "speculative transient budget mb",
            makeClampSanitizerInt(64, 16384) };

        // OpimizedMW GL-P1A is an independent opt-in, not part of the V3 preset.
        SettingValue<bool> mOpimizedMWHostPressure{ mIndex, "Cells", "host memory pressure" };
        SettingValue<int> mOpimizedMWHostReserveMb{ mIndex, "Cells", "host memory reserve mb",
            makeClampSanitizerInt(0, 65536) };
        SettingValue<int> mOpimizedMWCommitReserveMb{ mIndex, "Cells", "commit memory reserve mb",
            makeClampSanitizerInt(256, 65536) };

        // Descriptive settings share the existing category object. The parser
        // accepts historical names without changing their stored semantics.
        SettingValue<int> mExperimentalCacheBudgetMb{
            mIndex, "Optimizations", "experimental cache budget mb", makeClampSanitizerInt(1, 4096) };
        SettingValue<int> mExperimentalScratchBudgetMb{
            mIndex, "Optimizations", "experimental scratch budget mb", makeClampSanitizerInt(1, 4096) };
        SettingValue<int> mExperimentalConcurrentBuilds{
            mIndex, "Optimizations", "experimental concurrent builds", makeClampSanitizerInt(1, 2) };
        SettingValue<bool> mV36PerformanceProfile{ mIndex, "Optimizations", "legacy performance profile" };
        SettingValue<bool> mV36DisableRamOverdrive{ mIndex, "Optimizations", "disable legacy ram overdrive" };
        SettingValue<bool> mV36DisableLuaFastPath{ mIndex, "Optimizations", "disable legacy lua fast path" };
        SettingValue<bool> mV36DisableCoarseChunkOcclusion{
            mIndex, "Optimizations", "disable legacy coarse chunk occlusion" };
        SettingValue<bool> mV36AsyncGpuProfiler{ mIndex, "Optimizations", "async gpu profiler" };
        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "Shadows", "far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };
        // V3.19 stable gaming: promoted focus temporal-coherence cadence.
        SettingValue<int> mV319FocusCadence{
            mIndex, "Camera", "focus refresh interval frames", makeClampSanitizerInt(1, 3) };
        // V3.20: optionally force a focus refresh when the main camera contract changes.
        // Disabled by default so the promoted fixed-cadence P0 path remains exact.
        SettingValue<bool> mV320AdaptiveFocusCadence{
            mIndex, "Camera", "focus refresh on camera change" };
        // V3.21 CP1 downstream completed-work admission. Mode 0 preserves the
        // V3.20 behavior; mode 1 enables the fixed governor.
        SettingValue<int> mV321CompletionGovernorMode{
            mIndex, "Cells", "completed work governor mode", makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV321CompileObjectsPerFrame{
            mIndex, "Cells", "completed work compile objects per frame", makeClampSanitizerInt(1, 20) };
        SettingValue<int> mV321MergeSetsPerFrame{
            mIndex, "Cells", "completed work merge sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321MaxDeferredFrames{
            mIndex, "Cells", "completed work max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321ForcedMergeSets{
            mIndex, "Cells", "completed work forced merge sets", makeClampSanitizerInt(0, 8) };
        SettingValue<float> mV321CompileMinimumMilliseconds{
            mIndex, "Cells", "completed work compile minimum milliseconds", makeClampSanitizerFloat(0.1, 4.0) };
        SettingValue<float> mV321CompileConservativeRatio{
            mIndex, "Cells", "completed work compile conservative ratio", makeClampSanitizerFloat(0.1, 1.0) };
        // MODE 127 adaptive merge-admission controls. Adaptation uses only the
        // previously completed frame and a bounded EMA/debt state.
        SettingValue<float> mV321AdaptiveTargetMilliseconds{
            mIndex, "Cells", "completed work adaptive target milliseconds", makeClampSanitizerFloat(8.0, 50.0) };
        SettingValue<float> mV321AdaptiveFrameEmaAlpha{
            mIndex, "Cells", "completed work adaptive frame ema alpha", makeClampSanitizerFloat(0.05, 1.0) };
        SettingValue<int> mV321AdaptiveMergeMin{
            mIndex, "Cells", "completed work adaptive merge minimum", makeClampSanitizerInt(1, 8) };
        SettingValue<int> mV321AdaptiveMergeMax{
            mIndex, "Cells", "completed work adaptive merge maximum", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321AdaptiveDebtCap{
            mIndex, "Cells", "completed work adaptive debt cap", makeClampSanitizerInt(0, 32) };
        SettingValue<int> mV321AdaptiveDebtRepayPerFrame{
            mIndex, "Cells", "completed work adaptive debt repay per frame", makeClampSanitizerInt(0, 4) };
        // V3.21 CP2 class-aware completion fairness/dephasing. Default off.
        SettingValue<int> mV321CP2FairnessMode{
            mIndex, "Cells", "compile fairness mode", makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV321CP2ServiceSetsPerFrame{
            mIndex, "Cells", "compile fairness service sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2ClassBurstSetsPerFrame{
            mIndex, "Cells", "compile fairness class burst sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2MaxDeferredFrames{
            mIndex, "Cells", "compile fairness max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321CP2ForcedSets{
            mIndex, "Cells", "compile fairness forced sets", makeClampSanitizerInt(0, 8) };
        SettingValue<int> mV321CP2DeficitCap{
            mIndex, "Cells", "compile fairness deficit cap", makeClampSanitizerInt(1, 32) };
        SettingValue<bool> mV37DisableFarCasterPruning{
            mIndex, "Optimizations", "disable legacy far caster pruning" };
        SettingValue<bool> mV37ActiveEventFastPath{ mIndex, "Lua", "bulk active event fast path" };
        SettingValue<bool> mV37CompanionKeyframePreload{
            mIndex, "Cells", "companion keyframe preload" };
        SettingValue<bool> mV37RelaxedResourceSweep{
            mIndex, "Cells", "relaxed resource cache sweep" };
        SettingValue<float> mV37ResourceSweepSeconds{ mIndex, "Cells", "resource cache sweep seconds",
            makeClampSanitizerFloat(0.5, 60) };
        SettingValue<bool> mV37StabilizeFarCascade{ mIndex, "Shadows", "stabilize far cascade" };

        SettingValue<int> mV38WorldBatchingMode{ mIndex, "Cells", "world batching mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<float> mV38WorldBatchingMergeMultiplier{ mIndex, "Cells",
            "world batching merge multiplier", makeClampSanitizerFloat(1, 8) };
        SettingValue<int> mV38WorldBatchingMinInstances{ mIndex, "Cells",
            "world batching minimum instances", makeClampSanitizerInt(2, 64) };
        SettingValue<int> mV38GpuResidencyMode{ mIndex, "Cells", "gpu residency mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38FarShadowMode{ mIndex, "Shadows", "legacy far caster pruning mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV38CompilePacingMode{ mIndex, "Cells", "compile pacing mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<int> mV39FrontloadMode{ mIndex, "Cells", "initial multi-view preload mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39BatchOptimizerMode{ mIndex, "Cells", "batch optimizer mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39ProactiveResidencyMode{ mIndex, "Cells", "proactive residency mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<bool> mV310FreshInitialObjectPaging{ mIndex, "Cells",
            "fresh initial object paging" };
        SettingValue<bool> mV310PreloadPostTransform{ mIndex, "Cells", "initial preload post-transform" };

        SettingValue<int> mV311ActiveGridPrepareMode{ mIndex, "Cells", "active grid prepare mode",
            makeClampSanitizerInt(0, 2) };

        // Index-only tiers for independently published strong active-grid
        // controls. Required readiness and ordinary interaction retain control.
        SettingValue<bool> mGeneratedStaticLodActiveGrid{
            mIndex, "Cells", "generated static lod active grid" };

        SettingValue<int> mV312PredictorMode{ mIndex, "Cells", "boundary predictor mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<float> mV312PredictorLeadSeconds{ mIndex, "Cells", "boundary predictor lead seconds",
            makeClampSanitizerFloat(0.25, 8.0) };
        SettingValue<bool> mV312LuaPrecompile{ mIndex, "Lua", "precompile configured scripts" };
        SettingValue<int> mV312SpatialBatchMode{ mIndex, "Cells", "spatial batch mode",
            makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV313ChunkQualityMode{ mIndex, "Cells", "prepared chunk quality mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV314LuaDependencyPrecompileMode{ mIndex, "Lua", "dependency precompile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314LuaPackagePrototypeReuse{ mIndex, "Lua", "package prototype reuse" };
        SettingValue<int> mV314GroundcoverCompileMode{ mIndex, "Groundcover", "compile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314PostfxCompileWarmup{ mIndex, "Post Processing", "compile warmup" };

        SettingValue<bool> mV315PremergeStateCanonicalization{ mIndex, "Cells",
            "premerge state canonicalization" };
        SettingValue<int> mV315PacketizedPremergeMode{ mIndex, "Cells", "packetized premerge mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "Cells", "adaptive compile governor",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV316IdleResourceSweep{ mIndex, "Cells", "idle resource sweep" };
        SettingValue<bool> mV316SfxMetadataFrontload{ mIndex, "Sound", "preload sound metadata" };

        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme", "overdrive" }) };
        SettingValue<std::string> mRamCacheOverdrivePreload{ mIndex, "Cells", "ram cache overdrive preload",
            makeEnumSanitizerString({ "balanced", "aggressive", "maximum" }) };
        SettingValue<std::string> mV3StreamingScheduler{ mIndex, "Cells", "streaming scheduler",
            makeEnumSanitizerString({ "off", "adaptive", "adaptive-v2" }) };
        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mV32StreamingMaxDefers{ mIndex, "Cells", "streaming max defers",
            makeClampSanitizerInt(0, 16) };
        SettingValue<bool> mV3PreparedInstanceCache{ mIndex, "Cells", "prepared instance cache" };
        SettingValue<int> mV3PreparedInstanceCacheMax{ mIndex, "Cells", "prepared instance cache maximum",
            makeClampSanitizerInt(256, 65536) };
        SettingValue<bool> mV32ExteriorHibernation{ mIndex, "Cells", "exterior hibernation" };
        SettingValue<bool> mV32RendererInsertionProfiling{ mIndex, "Optimizations", "renderer insertion profiling" };
        SettingValue<bool> mV32GpuMemoryTelemetry{ mIndex, "Optimizations", "gpu memory telemetry" };
        SettingValue<bool> mV32GpuMemoryManagement{ mIndex, "Cells", "gpu memory management" };
        SettingValue<int> mV32GpuSoftBudgetMb{ mIndex, "Cells", "gpu soft budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV32GpuHardBudgetMb{ mIndex, "Cells", "gpu hard budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV33SpeculativePreloadBudget{ mIndex, "Cells", "speculative preload jobs",
            makeClampSanitizerInt(0, 64) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };
        SettingValue<int> mPreloadNumThreads{ mIndex, "Cells", "preload num threads", makeMaxSanitizerInt(1) };
        SettingValue<bool> mPreloadExteriorGrid{ mIndex, "Cells", "preload exterior grid" };
        SettingValue<bool> mPreloadFastTravel{ mIndex, "Cells", "preload fast travel" };
        SettingValue<bool> mPreloadDoors{ mIndex, "Cells", "preload doors" };
        SettingValue<float> mPreloadDistance{ mIndex, "Cells", "preload distance", makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mPreloadInstances{ mIndex, "Cells", "preload instances" };
        // Startup-only canonical publication for concurrent ordinary decoder misses.
        SettingValue<bool> mCanonicalImagePublication{ mIndex, "Cells", "canonical image publication" };
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
