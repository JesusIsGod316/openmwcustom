import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.9 frontload match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.9 frontloaded batching patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.9 design contract
#
# The user explicitly prefers a large one-time exterior initialization cost over
# recurring movement hitches. V3.9 therefore uses OpenMW's existing synchronous
# Terrain/QuadTree preload path to build ObjectPaging chunks for several nearby
# future viewpoints before normal play begins. QuadTreeWorld::preload already
# calls every ChunkManager (including ObjectPaging) with compile=true, so this
# reuses a mature engine path rather than inventing a second paging system.
#
# Frontload modes:
#   0 = off / exact V3.8 behavior
#   1 = center + four cardinal future views
#   2 = 3x3 future-view block (normal V3.9 candidate)
#   3 = 5x5 future-view block (aggressive; intentionally expensive startup)
#
# Batch optimizer modes preserve the SAME strong V3.8 merge admission. They only
# control expensive post-merge cleanup passes:
#   0 = exact V3.8 behavior (POSTTRANSFORM at >=2, PRETRANSFORM at 3, share state)
#   1 = merge-only fast path
#   2 = merge + SharedStateManager compaction
#   3 = merge + POSTTRANSFORM + SharedStateManager compaction (never PRETRANSFORM)
# This directly tests the hypothesis that general-purpose mesh reorder passes are
# responsible for much of V3.8 mode-2/3 construction cost while draw reduction
# primarily comes from MERGE_GEOMETRY itself.
#
# Proactive residency modes only alter SOFT-pressure cache-only expiration ages.
# Live/external nodes remain protected by GenericObjectCache reference counts and
# source NIF/image/keyframe caches retain Overdrive lifetimes.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV38CompilePacingMode{ mIndex, "V3", "v3.8 compile pacing mode",
            makeClampSanitizerInt(0, 3) };''',
    '''        SettingValue<int> mV38CompilePacingMode{ mIndex, "V3", "v3.8 compile pacing mode",
            makeClampSanitizerInt(0, 3) };

        SettingValue<int> mV39FrontloadMode{ mIndex, "V3", "v3.9 frontload mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39BatchOptimizerMode{ mIndex, "V3", "v3.9 batch optimizer mode",
            makeClampSanitizerInt(0, 3) };
        SettingValue<int> mV39ProactiveResidencyMode{ mIndex, "V3", "v3.9 proactive residency mode",
            makeClampSanitizerInt(0, 3) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.8 compile pacing mode = 0

[Cells]''',
    '''v3.8 compile pacing mode = 0

# V3.9 startup-frontload / traversal-smoothness controls.
# frontload: 0=off, 1=center+cardinals, 2=3x3 viewpoints, 3=5x5 viewpoints.
# The extra work is intentionally allowed to extend the initial loading screen so
# strong ObjectPaging batches are already built before ordinary movement.
v3.9 frontload mode = 0

# Post-merge optimizer policy. Strong V3.8 merge admission is unchanged.
# 0=exact V3.8, 1=merge only, 2=merge+shared-state, 3=merge+post-transform+shared-state.
v3.9 batch optimizer mode = 0

# Earlier cache-only render-graph reclamation while adapter pressure is Soft.
# 0=V3.8 behavior, 1=gentle, 2=balanced, 3=aggressive.
v3.9 proactive residency mode = 0

[Cells]''',
)

# One-time state belongs to Scene, not a static local, so it resets naturally
# when the world/Scene is recreated and cannot leak between game sessions.
replace_exact(
    "apps/openmw/mwworld/scene.hpp",
    '''        osg::Vec3f mLastPlayerPos;

        std::vector<ESM::RefNum> mPagedRefs;''',
    '''        osg::Vec3f mLastPlayerPos;

        // V3.9: perform the intentionally expensive multi-view exterior preload
        // only once per Scene lifetime. Subsequent cell-grid changes use normal
        // predictive/background preload and must not repeat startup frontloading.
        bool mV39InitialFrontloadDone = false;

        std::vector<ESM::RefNum> mPagedRefs;''',
)

# Guarantee that the first exterior grid load enters the synchronous preload even
# if a smaller background prediction happened to finish just before it.
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''        mPreloader->setTerrain(mRendering.getTerrain());
        if (mRendering.pagingUnlockCache())
            mPreloader->abortTerrainPreloadExcept(nullptr);
        if (!mPreloader->isTerrainLoaded(PositionCellGrid{ pos, newGrid }, mRendering.getReferenceTime()))
            preloadTerrain(pos, playerCellIndex.mWorldspace, true);
        mPagedRefs.clear();''',
    '''        mPreloader->setTerrain(mRendering.getTerrain());
        if (mRendering.pagingUnlockCache())
            mPreloader->abortTerrainPreloadExcept(nullptr);
        const bool v39NeedsInitialFrontload
            = static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !mV39InitialFrontloadDone;
        if (v39NeedsInitialFrontload
            || !mPreloader->isTerrainLoaded(PositionCellGrid{ pos, newGrid }, mRendering.getReferenceTime()))
            preloadTerrain(pos, playerCellIndex.mWorldspace, true);
        mPagedRefs.clear();''',
)

# Expand only the FIRST synchronous exterior preload. Each PositionCellGrid is an
# independent Terrain::View, and QuadTreeWorld::preload traverses all registered
# chunk managers with compile=true. This causes terrain + ObjectPaging/groundcover
# preparation to happen behind the loading screen instead of at later crossings.
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::preloadTerrain(const osg::Vec3f& pos, ESM::RefId worldspace, bool sync)
    {
        if (mRendering.getTerrain()->getWorldspace() != worldspace)
            throw std::runtime_error("preloadTerrain can only work with the current exterior worldspace");

        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
        const PositionCellGrid position{ pos, gridCenterToBounds({ cellPos.mX, cellPos.mY }) };
        mPreloader->abortTerrainPreloadExcept(&position);
        mPreloader->setTerrainPreloadPositions(std::span(&position, 1));
        if (!sync)
            return;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        loadingListener->setLabel("#{OMWEngine:InitializingData}");

        mPreloader->syncTerrainLoad(*loadingListener);
    }''',
    '''    void Scene::preloadTerrain(const osg::Vec3f& pos, ESM::RefId worldspace, bool sync)
    {
        if (mRendering.getTerrain()->getWorldspace() != worldspace)
            throw std::runtime_error("preloadTerrain can only work with the current exterior worldspace");

        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);
        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;

        std::vector<PositionCellGrid> positions;
        if (v39DoFrontload)
        {
            // Cancel a potentially smaller prediction task so the startup task is
            // guaranteed to contain the full requested future-view set.
            mPreloader->abortTerrainPreloadExcept(nullptr);

            const int cellSize = ESM::getCellSize(worldspace);
            // Step beyond the current active grid. A single QuadTree view already
            // covers distant LODs, so adjacent future viewpoints overlap heavily
            // and prime the chunks most likely to be encountered during traversal.
            const int stepCells = std::max(1, mHalfGridSize + 1);

            const auto addView = [&](int dxCells, int dyCells) {
                osg::Vec3f preloadPos = pos;
                preloadPos.x() += static_cast<float>(dxCells * cellSize);
                preloadPos.y() += static_cast<float>(dyCells * cellSize);
                const ESM::ExteriorCellLocation preloadCell = ESM::positionToExteriorCellLocation(
                    preloadPos.x(), preloadPos.y(), worldspace);
                positions.push_back(PositionCellGrid{
                    preloadPos, gridCenterToBounds(osg::Vec2i(preloadCell.mX, preloadCell.mY)) });
            };

            addView(0, 0);
            if (v39FrontloadMode == 1)
            {
                addView(stepCells, 0);
                addView(-stepCells, 0);
                addView(0, stepCells);
                addView(0, -stepCells);
            }
            else
            {
                const int radius = v39FrontloadMode >= 3 ? 2 : 1;
                for (int y = -radius; y <= radius; ++y)
                {
                    for (int x = -radius; x <= radius; ++x)
                    {
                        if (x == 0 && y == 0)
                            continue;
                        addView(x * stepCells, y * stepCells);
                    }
                }
            }

            mPreloader->setTerrainPreloadPositions(positions);
        }
        else
        {
            ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
            const PositionCellGrid position{ pos, gridCenterToBounds({ cellPos.mX, cellPos.mY }) };
            mPreloader->abortTerrainPreloadExcept(&position);
            mPreloader->setTerrainPreloadPositions(std::span(&position, 1));
        }

        if (!sync)
            return;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        loadingListener->setLabel("#{OMWEngine:InitializingData}");
        mPreloader->syncTerrainLoad(*loadingListener);

        if (v39DoFrontload)
            mV39InitialFrontloadDone = true;
    }''',
)

# Replace V3.8's unconditional expensive mode-2/3 mesh reorder policy with a
# separately selectable post-merge policy. MERGE_GEOMETRY itself remains enabled
# in every policy, so the large draw/submission reduction can survive while we
# remove work that does not contribute to drawable count.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            // These passes operate on the worker-built merged geometry, preserving
            // rendered content while improving post-transform cache locality. Keep the
            // more expensive access-order pass for the aggressive profile.
            if (v38BatchingMode >= 2)
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            if (v38BatchingMode >= 3)
                options |= SceneUtil::Optimizer::VERTEX_PRETRANSFORM;

            optimizer.optimize(mergeGroup, options);

            if (v38BatchingMode >= 2)
                mSceneManager->shareState(mergeGroup);''',
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const int v39BatchOptimizerMode = static_cast<int>(Settings::cells().mV39BatchOptimizerMode);

            if (v39BatchOptimizerMode == 0)
            {
                // Exact V3.8 behavior for rollback/A-B comparison.
                if (v38BatchingMode >= 2)
                    options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
                if (v38BatchingMode >= 3)
                    options |= SceneUtil::Optimizer::VERTEX_PRETRANSFORM;
            }
            else if (v39BatchOptimizerMode >= 3 && v38BatchingMode >= 2)
            {
                // Retain only the cheaper locality pass. VERTEX_PRETRANSFORM is
                // deliberately excluded from all V3.9 optimized policies.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }

            optimizer.optimize(mergeGroup, options);

            const bool v39ShareState = v39BatchOptimizerMode >= 2;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);''',
)

# V3.9 may reclaim cache-only render graphs earlier in the Soft band. The V3.8
# hard-pressure policy remains authoritative; this only changes Soft pressure and
# therefore adds hysteresis/headroom before the adapter reaches the hard limit.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                if (interval > 0.0 && now - sV38LastResidencyTrim >= interval)
                {''',
    '''                const int v39ProactiveResidencyMode
                    = static_cast<int>(Settings::cells().mV39ProactiveResidencyMode);
                if (v39ProactiveResidencyMode > 0
                    && pressure == Debug::V3GpuMemory::PressureState::Soft)
                {
                    if (v39ProactiveResidencyMode == 1)
                    {
                        interval = 1.25;
                        sceneAge = 45.0;
                        pagingAge = 60.0;
                    }
                    else if (v39ProactiveResidencyMode == 2)
                    {
                        interval = 0.75;
                        sceneAge = 20.0;
                        pagingAge = 30.0;
                    }
                    else
                    {
                        interval = 0.5;
                        sceneAge = 8.0;
                        pagingAge = 12.0;
                    }
                }

                if (interval > 0.0 && now - sV38LastResidencyTrim >= interval)
                {''',
)

# -----------------------------------------------------------------------------
# Compact runtime matrix. 55 is an exact V3.8-safe reference. 56 isolates the
# core frontloaded strong-merge idea. 57 is the intended normal V3.9 candidate.
# 58 intentionally spends much more startup time to maximize future coverage.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V38CompilePacingMode = '0'
$RendererProfiling''',
    '''$V38CompilePacingMode = '0'
$V39FrontloadMode = '0'
$V39BatchOptimizerMode = '0'
$V39ProactiveResidencyMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
do { $choice = Read-Host 'Enter 1 through 54' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54'))''',
    '''Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
Write-Host ' 55 = V3.9 V3.8-safe reference (Mode 46 equivalent)'
Write-Host ' 56 = V3.9 frontloaded strong batching'
Write-Host ' 57 = V3.9 combined candidate'
Write-Host ' 58 = V3.9 aggressive frontload / batching / residency'
do { $choice = Read-Host 'Enter 1 through 58' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '54' { $Experiment = 'v38-compile-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '3' }
}''',
    '''    '54' { $Experiment = 'v38-compile-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '3' }
    '55' { $Experiment = 'v39-v38-safe-reference'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '1' }
    '56' { $Experiment = 'v39-frontloaded-batching'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1' }
    '57' { $Experiment = 'v39-combined-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '2'; $V39ProactiveResidencyMode = '2' }
    '58' { $Experiment = 'v39-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '3'; $V39ProactiveResidencyMode = '3' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v38_compile_pacing_mode=$V38CompilePacingMode",
    "shadow_distance=$ShadowDistance",''',
    '''    "v38_compile_pacing_mode=$V38CompilePacingMode",
    "v39_frontload_mode=$V39FrontloadMode",
    "v39_batch_optimizer_mode=$V39BatchOptimizerMode",
    "v39_proactive_residency_mode=$V39ProactiveResidencyMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' $V38CompilePacingMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' $V38CompilePacingMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 frontload mode' $V39FrontloadMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 batch optimizer mode' $V39BatchOptimizerMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 proactive residency mode' $V39ProactiveResidencyMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.9 frontloaded batching / proactive residency stack patched successfully.")
