import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.11 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.11 adjacent active-grid patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.11 design contract
#
# V3.8 Mode47 proved that strong live ObjectPaging chunks can cut draw/GPU cost,
# but constructing them synchronously causes unacceptable traversal hitches.
# V3.9/V3.10 moved strong work to preload, yet the startup pattern jumped by
# mHalfGridSize+1 cells and therefore skipped the immediate +/-1 grid-center states
# that normal traversal actually enters. V3.11 keeps the safe compile=false Mode1
# fallback and instead drives strong PREPARED active-grid hit rate toward 100%.
#
# Active-grid prepare mode:
#   0 = inherited V3.10 behavior
#   1 = strong compile=true activeGrid chunks get shared-state compaction
#   2 = same exact population + VERTEX_POSTTRANSFORM
# Strong merge admission itself still comes from V3.8 world batching mode 2.
# V3.11 never promotes compile=false demand misses and never enables
# VERTEX_PRETRANSFORM.
# -----------------------------------------------------------------------------

replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<bool> mV310FreshInitialObjectPaging{ mIndex, "V3",
            "v3.10 fresh initial object paging" };
        SettingValue<bool> mV310PreloadPostTransform{ mIndex, "V3", "v3.10 preload post-transform" };''',
    '''        SettingValue<bool> mV310FreshInitialObjectPaging{ mIndex, "V3",
            "v3.10 fresh initial object paging" };
        SettingValue<bool> mV310PreloadPostTransform{ mIndex, "V3", "v3.10 preload post-transform" };

        SettingValue<int> mV311ActiveGridPrepareMode{ mIndex, "V3", "v3.11 active grid prepare mode",
            makeClampSanitizerInt(0, 2) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# When true, the fresh one-time initial frontload receives VERTEX_POSTTRANSFORM
# vertex-cache ordering plus shared-state compaction. Later predictive/background
# preload and synchronous gameplay-demand misses do not receive this V3.10 pass.
# VERTEX_PRETRANSFORM is never enabled by this switch.
v3.10 preload post-transform = false

[Cells]''',
    '''# When true, the fresh one-time initial frontload receives VERTEX_POSTTRANSFORM
# vertex-cache ordering plus shared-state compaction. Later predictive/background
# preload and synchronous gameplay-demand misses do not receive this V3.10 pass.
# VERTEX_PRETRANSFORM is never enabled by this switch.
v3.10 preload post-transform = false

# V3.11 exact active-grid preparation. 0=off/inherited behavior,
# 1=strong prepared active-grid chunks + shared-state compaction,
# 2=same exact population + VERTEX_POSTTRANSFORM. Demand misses remain Mode1.
v3.11 active grid prepare mode = 0

[Cells]''',
)

# Startup coverage correction: when V3.11 is enabled, the nominal 3x3 frontload
# means the current grid center plus the eight IMMEDIATE neighboring grid centers.
# Older V3.9/V3.10 modes preserve their historical step=mHalfGridSize+1 behavior.
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''            // Step beyond the current active grid. A single QuadTree view already
            // covers distant LODs, so adjacent future viewpoints overlap heavily
            // and prime the chunks most likely to be encountered during traversal.
            const int stepCells = std::max(1, mHalfGridSize + 1);''',
    '''            // V3.11 prepares the exact neighboring grid-center states normal
            // traversal is most likely to enter next. Older modes retain the
            // historical wider jump for strict backward comparison.
            const bool v311ExactActiveGrid
                = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0;
            const int stepCells = v311ExactActiveGrid ? 1 : std::max(1, mHalfGridSize + 1);''',
)

# Retain the newest meaningful predicted grid target instead of silently dropping
# it while an expensive strong TerrainPreloadItem is still running. We only retarget
# when the FIRST position's cell-grid bounds change, so high-frequency predictedPos
# jitter within the same future grid cannot create cancellation/requeue thrash.
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''#include <components/resource/scenemanager.hpp>
#include <components/terrain/view.hpp>''',
    '''#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.hpp",
    '''        std::vector<PositionCellGrid> mTerrainPreloadPositions;
        osg::ref_ptr<TerrainPreloadItem> mTerrainPreloadItem;
        osg::ref_ptr<SceneUtil::WorkItem> mUpdateCacheItem;

        std::vector<PositionCellGrid> mLoadedTerrainPositions;''',
    '''        std::vector<PositionCellGrid> mTerrainPreloadPositions;
        std::vector<PositionCellGrid> mV311PendingTerrainPreloadPositions;
        osg::ref_ptr<TerrainPreloadItem> mTerrainPreloadItem;
        osg::ref_ptr<SceneUtil::WorkItem> mUpdateCacheItem;

        std::vector<PositionCellGrid> mLoadedTerrainPositions;''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.hpp",
    '''        std::size_t mExpired = 0;
        std::size_t mLoaded = 0;''',
    '''        std::size_t mExpired = 0;
        std::size_t mLoaded = 0;

        std::size_t mV311TerrainTargetCompleted = 0;
        std::size_t mV311TerrainTargetReplaced = 0;
        std::size_t mV311TerrainTargetPromoted = 0;''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        if (mTerrainPreloadItem && mTerrainPreloadItem->isDone())
        {
            mLoadedTerrainPositions = mTerrainPreloadPositions;
            mLoadedTerrainTimestamp = timestamp;
        }''',
    '''        if (mTerrainPreloadItem && mTerrainPreloadItem->isDone())
        {
            mLoadedTerrainPositions = mTerrainPreloadPositions;
            mLoadedTerrainTimestamp = timestamp;

            if (static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0)
            {
                ++mV311TerrainTargetCompleted;
                if (!mV311PendingTerrainPreloadPositions.empty())
                {
                    std::vector<PositionCellGrid> pending = std::move(mV311PendingTerrainPreloadPositions);
                    mV311PendingTerrainPreloadPositions.clear();
                    ++mV311TerrainTargetPromoted;
                    setTerrainPreloadPositions(pending);
                }
            }
        }''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''    void CellPreloader::setTerrainPreloadPositions(std::span<const PositionCellGrid> positions)
    {
        if (positions.empty())
        {
            mTerrainPreloadPositions.clear();
            mLoadedTerrainPositions.clear();
        }
        else if (contains(mTerrainPreloadPositions, positions, 128.f))
            return;
        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
            return;
        else
        {''',
    '''    void CellPreloader::setTerrainPreloadPositions(std::span<const PositionCellGrid> positions)
    {
        const bool v311RollingExactActive
            = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0;

        if (positions.empty())
        {
            mTerrainPreloadPositions.clear();
            mLoadedTerrainPositions.clear();
            mV311PendingTerrainPreloadPositions.clear();
        }
        else if (contains(mTerrainPreloadPositions, positions, 128.f))
            return;

        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            if (!v311RollingExactActive || positions.empty())
                return;

            const auto firstBoundsEqual = [](std::span<const PositionCellGrid> a,
                                              std::span<const PositionCellGrid> b) {
                return !a.empty() && !b.empty() && a.front().mCellBounds == b.front().mCellBounds;
            };

            // The running item already targets this exact future grid. Ignore
            // predicted-position jitter until the grid bounds themselves change.
            if (firstBoundsEqual(mTerrainPreloadPositions, positions))
                return;

            // Keep exactly one newest future-grid target. If prediction changes
            // again before the old worker finishes, replace the pending target.
            if (!mV311PendingTerrainPreloadPositions.empty()
                && !firstBoundsEqual(mV311PendingTerrainPreloadPositions, positions))
                ++mV311TerrainTargetReplaced;

            mV311PendingTerrainPreloadPositions.assign(positions.begin(), positions.end());
            return;
        }
        else
        {''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        if (mTerrainPreloadItem)
        {
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
            mTerrainPreloadItem = nullptr;
        }''',
    '''        if (mTerrainPreloadItem)
        {
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
            mTerrainPreloadItem = nullptr;
        }
        mV311PendingTerrainPreloadPositions.clear();''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        stats.setAttribute(frameNumber, "CellPreloader Expired", static_cast<double>(mExpired));
    }''',
    '''        stats.setAttribute(frameNumber, "CellPreloader Expired", static_cast<double>(mExpired));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Completed",
            static_cast<double>(mV311TerrainTargetCompleted));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Replaced",
            static_cast<double>(mV311TerrainTargetReplaced));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Promoted",
            static_cast<double>(mV311TerrainTargetPromoted));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Pending",
            mV311PendingTerrainPreloadPositions.empty() ? 0.0 : 1.0);
    }''',
)

# Track exactly which cache entries were generated as strong V3.11 active-grid
# preparations. This lets runtime logs distinguish algorithm failure from predictor
# miss. A compile=false active-grid cache miss increments the fallback counter.
replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        std::atomic_bool mV310InitialFrontloadActive{ false };

        std::mutex mRefTrackerMutex;''',
    '''        std::atomic_bool mV310InitialFrontloadActive{ false };

        mutable std::mutex mV311PreparedActiveMutex;
        std::set<ChunkId> mV311PreparedActiveChunks;
        std::atomic_uint64_t mV311PreparedActiveBuilt{ 0 };
        std::atomic_uint64_t mV311PreparedActiveHits{ 0 };
        std::atomic_uint64_t mV311DemandFallbacks{ 0 };

        std::mutex mRefTrackerMutex;''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const ChunkId id = std::make_tuple(center, size, activeGrid);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            return static_cast<osg::Node*>(obj.get());

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());
        return node;''',
    '''        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
        {
            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                if (mV311PreparedActiveChunks.contains(id))
                    mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
            }
            return static_cast<osg::Node*>(obj.get());
        }

        if (v311PrepareMode > 0 && activeGrid && !compile)
        {
            mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.erase(id);
        }

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());

        if (v311PrepareMode > 0 && activeGrid && compile)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;''',
)

# V3.11 applies post-merge cleanup ONLY to strong compile=true active-grid chunks.
# Strong merge admission remains the V3.8 mode-2 policy. Non-active distant chunks
# retain V3.9 merge-only cleanup, and compile=false demand misses remain cheap.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const bool v310PreloadPostTransform
                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)
                && compile && mV310InitialFrontloadActive.load(std::memory_order_acquire)
                && v38BatchingMode >= 2;

            if (v39BatchOptimizerMode == 0 && !v310PreloadPostTransform)''',
    '''            const bool v310PreloadPostTransform
                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)
                && compile && mV310InitialFrontloadActive.load(std::memory_order_acquire)
                && v38BatchingMode >= 2;
            const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
            const bool v311PreparedActive = v311PrepareMode > 0 && compile && activeGrid && v38BatchingMode >= 2;
            const bool v311PreparedPostTransform = v311PrepareMode >= 2 && v311PreparedActive;

            if (v39BatchOptimizerMode == 0 && !v310PreloadPostTransform && !v311PreparedActive)''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            else if ((v39BatchOptimizerMode >= 3 || v310PreloadPostTransform) && v38BatchingMode >= 2)
            {
                // V3.10 may explicitly promote this locality pass on its startup work.
                // The V3.10 override never requests VERTEX_PRETRANSFORM.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }''',
    '''            else if ((v39BatchOptimizerMode >= 3 || v310PreloadPostTransform || v311PreparedPostTransform)
                && v38BatchingMode >= 2)
            {
                // V3.10 may promote startup work; V3.11 may promote only exact
                // compile=true active-grid preparation. Neither path requests
                // VERTEX_PRETRANSFORM.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const bool v39ShareState = v39BatchOptimizerMode >= 2 || v310PreloadPostTransform;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);''',
    '''            const bool v39ShareState
                = v39BatchOptimizerMode >= 2 || v310PreloadPostTransform || v311PreparedActive;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''    void ObjectPaging::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Object Chunk", frameNumber, mCache->getStats(), *stats);
    }''',
    '''    void ObjectPaging::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Object Chunk", frameNumber, mCache->getStats(), *stats);
        stats->setAttribute(frameNumber, "V3.11 Prepared Active Built",
            static_cast<double>(mV311PreparedActiveBuilt.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.11 Prepared Active Hit",
            static_cast<double>(mV311PreparedActiveHits.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.11 Demand Fallback",
            static_cast<double>(mV311DemandFallbacks.load(std::memory_order_relaxed)));
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            stats->setAttribute(frameNumber, "V3.11 Prepared Active Resident",
                static_cast<double>(mV311PreparedActiveChunks.size()));
        }
    }''',
)

# -----------------------------------------------------------------------------
# Launcher matrix. Existing Modes56/59 remain useful inherited references.
# 63 is FIRST TEST: exact adjacent/rolling active-grid strong preparation with
# shared-state compaction but no post-transform. 64 is the identical population
# plus post-transform. 65/66 layer the already visually-clean 5px far-shadow mode
# so either winner can be tested as a combined candidate without another rebuild.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V310FreshInitialObjectPaging = 'false'
$V310PreloadPostTransform = 'false'
$RendererProfiling''',
    '''$V310FreshInitialObjectPaging = 'false'
$V310PreloadPostTransform = 'false'
$V311ActiveGridPrepareMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 59 = V3.10 fresh-frontload control (no post-transform)'
Write-Host ' 60 = V3.10 fresh frontload + post-transform 3x3 (FIRST TEST)'
Write-Host ' 61 = V3.10 combined post-transform + 5px far shadow'
Write-Host ' 62 = V3.10 post-transform 5x5 startup coverage'
do { $choice = Read-Host 'Enter 1 through 62' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62'))''',
    '''Write-Host ' 59 = V3.10 fresh-frontload control (no post-transform)'
Write-Host ' 60 = V3.10 fresh frontload + post-transform 3x3'
Write-Host ' 61 = V3.10 combined post-transform + 5px far shadow'
Write-Host ' 62 = V3.10 post-transform 5x5 startup coverage'
Write-Host ' 63 = V3.11 exact active-grid strong + shared state (FIRST TEST)'
Write-Host ' 64 = V3.11 exact active-grid strong + post-transform'
Write-Host ' 65 = V3.11 Mode63 + 5px far shadow combined'
Write-Host ' 66 = V3.11 Mode64 + 5px far shadow combined'
do { $choice = Read-Host 'Enter 1 through 66' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '62' { $Experiment = 'v310-posttransform-5x5'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V310PreloadPostTransform = 'true' }
}''',
    '''    '62' { $Experiment = 'v310-posttransform-5x5'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V310PreloadPostTransform = 'true' }
    '63' { $Experiment = 'v311-exact-active-shared'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '1' }
    '64' { $Experiment = 'v311-exact-active-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '65' { $Experiment = 'v311-combined-shared'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '1' }
    '66' { $Experiment = 'v311-combined-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v310_preload_posttransform=$V310PreloadPostTransform",
    "shadow_distance=$ShadowDistance",''',
    '''    "v310_preload_posttransform=$V310PreloadPostTransform",
    "v311_active_grid_prepare_mode=$V311ActiveGridPrepareMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.10 preload post-transform' $V310PreloadPostTransform
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.10 preload post-transform' $V310PreloadPostTransform
    Set-IniValue $SettingsPath 'V3' 'v3.11 active grid prepare mode' $V311ActiveGridPrepareMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.11 adjacent/rolling exact active-grid preparation patched successfully.")
