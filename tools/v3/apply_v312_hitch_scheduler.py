import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.12 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.12 patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.12 controls. Mode66 remains the exact control; every new behavior is opt-in.
# predictor mode: 0=V3.11, 1=ETA-imminent grid first, 2=ETA first + ordinary
# predicted grid as a lower-priority second horizon when different.
# lua precompile only compiles/dumps configured top-level script bytecode. It does
# not create sandboxes, run script bodies, call onInit/onLoad, or materialize a
# ScriptsContainer.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV311ActiveGridPrepareMode{ mIndex, "V3", "v3.11 active grid prepare mode",
            makeClampSanitizerInt(0, 2) };''',
    '''        SettingValue<int> mV311ActiveGridPrepareMode{ mIndex, "V3", "v3.11 active grid prepare mode",
            makeClampSanitizerInt(0, 2) };

        SettingValue<int> mV312PredictorMode{ mIndex, "V3", "v3.12 predictor mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<float> mV312PredictorLeadSeconds{ mIndex, "V3", "v3.12 predictor lead seconds",
            makeClampSanitizerFloat(0.25, 8.0) };
        SettingValue<bool> mV312LuaPrecompile{ mIndex, "V3", "v3.12 lua precompile" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# V3.11 exact active-grid preparation. 0=off/inherited behavior,
# 1=strong prepared active-grid chunks + shared-state compaction,
# 2=same exact population + VERTEX_POSTTRANSFORM. Demand misses remain Mode1.
v3.11 active grid prepare mode = 0

[Cells]''',
    '''# V3.11 exact active-grid preparation. 0=off/inherited behavior,
# 1=strong prepared active-grid chunks + shared-state compaction,
# 2=same exact population + VERTEX_POSTTRANSFORM. Demand misses remain Mode1.
v3.11 active grid prepare mode = 0

# V3.12 traversal/Lua refinements. All default off so Mode66 is an exact control.
# Predictor mode 1 puts an imminent adjacent grid (estimated by time-to-boundary)
# first. Mode 2 may add the ordinary prediction as a second, lower-priority horizon.
v3.12 predictor mode = 0
v3.12 predictor lead seconds = 3.0
# Precompile configured Lua source to the existing bytecode cache during loading.
# This never runs a script body or onInit/onLoad.
v3.12 lua precompile = false

[Cells]''',
)

# -----------------------------------------------------------------------------
# Semantics-safe Lua bytecode precompile. loadScriptAndCache already proves the
# cache format/path; this method only populates missing entries while inside a
# protected Lua context. A bad/unused script remains non-fatal here and will retain
# upstream error behavior if normal execution later reaches it.
# -----------------------------------------------------------------------------
replace_exact(
    "components/lua/luastate.hpp",
    '''        void dropScriptCache() { mCompiledScripts.clear(); }

        const ScriptsConfiguration& getConfiguration() const { return *mConf; }''',
    '''        void dropScriptCache() { mCompiledScripts.clear(); }

        // V3.12: compile configured top-level scripts into the existing bytecode
        // cache without creating a sandbox or executing script code.
        std::size_t precompileConfiguredScripts();

        const ScriptsConfiguration& getConfiguration() const { return *mConf; }''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''    sol::function LuaState::loadScriptAndCache(const VFS::Path::Normalized& path)
    {
        auto iter = mCompiledScripts.find(path);''',
    '''    std::size_t LuaState::precompileConfiguredScripts()
    {
        std::size_t compiled = 0;
        protectedCall([&](LuaView&) {
            for (std::size_t i = 0; i < mConf->size(); ++i)
            {
                const VFS::Path::Normalized& path = (*mConf)[i].mScriptPath;
                if (mCompiledScripts.contains(path))
                    continue;
                try
                {
                    sol::function function = loadFromVFS(path);
                    mCompiledScripts[path] = function.dump();
                    ++compiled;
                }
                catch (const std::exception& e)
                {
                    // Prewarming must not make an otherwise-unused bad script a
                    // startup-fatal error. Normal execution keeps upstream behavior.
                    Log(Debug::Warning) << "V3.12 Lua precompile skipped " << path << ": " << e.what();
                }
            }
        });
        return compiled;
    }

    sol::function LuaState::loadScriptAndCache(const VFS::Path::Normalized& path)
    {
        auto iter = mCompiledScripts.find(path);''',
)

replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    void LuaManager::contentFilesLoaded()
    {
        initConfiguration(false);
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());''',
    '''    void LuaManager::contentFilesLoaded()
    {
        initConfiguration(false);
        if (static_cast<bool>(Settings::cells().mV312LuaPrecompile))
        {
            const std::size_t compiled = mLua.precompileConfiguredScripts();
            Log(Debug::Info) << "V3.12 Lua precompiled " << compiled << " configured script(s)";
        }
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());''',
)

# -----------------------------------------------------------------------------
# ETA-aware exact-grid predictor.
#
# V3.11 proved target accuracy but strong POSTTRANSFORM preparation can still miss
# roughly the first ~80 active chunks before settling. The old predictor extrapolates
# position by a fixed number of seconds. V3.12 instead estimates when current x/y
# motion will cross getNewGridCenter's exact threshold. When that boundary is near,
# the adjacent grid becomes FIRST in the TerrainPreloadItem, whose doWork() consumes
# positions in vector order. Mode2 can additionally retain the old predicted grid as
# a lower-priority second horizon. Required/demand work is never deferred.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwworld/scene.hpp",
    '''        osg::Vec3f mLastPlayerPos;

        std::vector<ESM::RefNum> mPagedRefs;''',
    '''        osg::Vec3f mLastPlayerPos;

        std::uint64_t mV312EtaTargets = 0;
        std::uint64_t mV312SecondHorizonTargets = 0;

        std::vector<ESM::RefNum> mPagedRefs;''',
)

replace_exact(
    "apps/openmw/mwworld/scene.hpp",
    '''#include <memory>
#include <optional>''',
    '''#include <cstdint>
#include <memory>
#include <optional>''',
)

replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''        if (mCurrentCell->isExterior())
            exteriorPositions.push_back(PositionCellGrid{
                predictedPos, gridCenterToBounds(getNewGridCenter(predictedPos, &mCurrentGridCenter)) });

        mLastPlayerPos = playerPos;''',
    '''        if (mCurrentCell->isExterior())
        {
            const int v312PredictorMode = static_cast<int>(Settings::cells().mV312PredictorMode);
            const osg::Vec2i normalPredictedGrid = getNewGridCenter(predictedPos, &mCurrentGridCenter);
            bool etaTargetAdded = false;

            if (v312PredictorMode > 0)
            {
                const osg::Vec3f velocity = moved / dt;
                const ESM::RefId worldspace = mCurrentCell->getCell()->getWorldSpace();
                const osg::Vec2f gridCenter = ESM::indexToPosition(ESM::ExteriorCellLocation(
                    mCurrentGridCenter.x(), mCurrentGridCenter.y(), worldspace), true);
                const float threshold = ESM::getCellSize(worldspace) / 2.f + mCellLoadingThreshold;
                const float leadSeconds = static_cast<float>(Settings::cells().mV312PredictorLeadSeconds);
                constexpr float MinVelocity = 1.f;
                float eta = std::numeric_limits<float>::infinity();

                const auto axisEta = [&](float position, float center, float speed) {
                    if (speed > MinVelocity)
                        return (center + threshold - position) / speed;
                    if (speed < -MinVelocity)
                        return (center - threshold - position) / speed;
                    return std::numeric_limits<float>::infinity();
                };

                const float etaX = axisEta(playerPos.x(), gridCenter.x(), velocity.x());
                const float etaY = axisEta(playerPos.y(), gridCenter.y(), velocity.y());
                if (etaX >= 0.f)
                    eta = std::min(eta, etaX);
                if (etaY >= 0.f)
                    eta = std::min(eta, etaY);

                if (eta <= leadSeconds)
                {
                    osg::Vec3f boundaryProbe = playerPos + velocity * eta;
                    osg::Vec2f horizontalVelocity(velocity.x(), velocity.y());
                    if (horizontalVelocity.length2() > 1.f)
                    {
                        horizontalVelocity.normalize();
                        boundaryProbe.x() += horizontalVelocity.x() * 64.f;
                        boundaryProbe.y() += horizontalVelocity.y() * 64.f;
                    }

                    const osg::Vec2i etaGrid = getNewGridCenter(boundaryProbe, &mCurrentGridCenter);
                    if (etaGrid != mCurrentGridCenter)
                    {
                        exteriorPositions.push_back(
                            PositionCellGrid{ boundaryProbe, gridCenterToBounds(etaGrid) });
                        etaTargetAdded = true;
                        ++mV312EtaTargets;

                        if (v312PredictorMode >= 2 && normalPredictedGrid != etaGrid
                            && normalPredictedGrid != mCurrentGridCenter)
                        {
                            exteriorPositions.push_back(
                                PositionCellGrid{ predictedPos, gridCenterToBounds(normalPredictedGrid) });
                            ++mV312SecondHorizonTargets;
                        }
                    }
                }
            }

            if (!etaTargetAdded)
                exteriorPositions.push_back(
                    PositionCellGrid{ predictedPos, gridCenterToBounds(normalPredictedGrid) });
        }

        mLastPlayerPos = playerPos;''',
)

replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        mPreloader->reportStats(frameNumber, stats);
    }''',
    '''    void Scene::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        mPreloader->reportStats(frameNumber, stats);
        stats.setAttribute(frameNumber, "V3.12 ETA Target Selected", static_cast<double>(mV312EtaTargets));
        stats.setAttribute(frameNumber, "V3.12 Second Horizon Target",
            static_cast<double>(mV312SecondHorizonTargets));
    }''',
)

# -----------------------------------------------------------------------------
# Compact runtime matrix. 67 is an exact Mode66 control. 68 isolates ETA/deadline
# ordering; 69 isolates Lua precompile; 70 is the safe combined candidate. Modes
# 71/72 are intentionally left for the spatial-clustering/CPU-horizon layer that
# will be added before V3.12 is finalized.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V311ActiveGridPrepareMode = '0'
$RendererProfiling''',
    '''$V311ActiveGridPrepareMode = '0'
$V312PredictorMode = '0'
$V312PredictorLeadSeconds = '3.0'
$V312LuaPrecompile = 'false'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 66 = V3.11 Mode64 + 5px far shadow combined'
do { $choice = Read-Host 'Enter 1 through 66' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66'))''',
    '''Write-Host ' 66 = V3.11 Mode64 + 5px far shadow combined'
Write-Host ' 67 = V3.12 exact Mode66 control'
Write-Host ' 68 = V3.12 Mode66 + ETA/deadline predictor'
Write-Host ' 69 = V3.12 Mode66 + safe Lua bytecode precompile'
Write-Host ' 70 = V3.12 combined ETA predictor + Lua precompile (FIRST SAFE CANDIDATE)'
do { $choice = Read-Host 'Enter 1 through 70' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66','67','68','69','70'))''',
)

mode66 = '''    '66' { $Experiment = 'v311-combined-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
}'''
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    mode66,
    '''    '66' { $Experiment = 'v311-combined-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '67' { $Experiment = 'v312-mode66-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '68' { $Experiment = 'v312-eta-predictor'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0' }
    '69' { $Experiment = 'v312-lua-precompile'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true' }
    '70' { $Experiment = 'v312-combined-safe'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v311_active_grid_prepare_mode=$V311ActiveGridPrepareMode",
    "shadow_distance=$ShadowDistance",''',
    '''    "v311_active_grid_prepare_mode=$V311ActiveGridPrepareMode",
    "v312_predictor_mode=$V312PredictorMode",
    "v312_predictor_lead_seconds=$V312PredictorLeadSeconds",
    "v312_lua_precompile=$V312LuaPrecompile",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.11 active grid prepare mode' $V311ActiveGridPrepareMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.11 active grid prepare mode' $V311ActiveGridPrepareMode
    Set-IniValue $SettingsPath 'V3' 'v3.12 predictor mode' $V312PredictorMode
    Set-IniValue $SettingsPath 'V3' 'v3.12 predictor lead seconds' $V312PredictorLeadSeconds
    Set-IniValue $SettingsPath 'V3' 'v3.12 lua precompile' $V312LuaPrecompile
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.12 ETA/deadline predictor + semantics-safe Lua precompile layer completed successfully.")
