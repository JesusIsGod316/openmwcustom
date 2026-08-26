import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.3 frame-pacing/GPU match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.3 frame-pacing/GPU patched {rel}")


replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV32GpuHardBudgetMb{ mIndex, "Cells", "v3.2 gpu hard budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        SettingValue<int> mV32GpuHardBudgetMb{ mIndex, "Cells", "v3.2 gpu hard budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV33SpeculativePreloadBudget{ mIndex, "Cells", "v3.3 speculative preload budget",
            makeClampSanitizerInt(0, 64) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
)

replace_once(
    "components/settings/categories/shadows.hpp",
    '''        SettingValue<int> mShadowMapResolution{ mIndex, "Shadows", "shadow map resolution" };
        SettingValue<float> mMinimumLispsmNearFarRatio{ mIndex, "Shadows", "minimum lispsm near far ratio",''',
    '''        SettingValue<int> mShadowMapResolution{ mIndex, "Shadows", "shadow map resolution" };
        SettingValue<int> mV33FarCascadeUpdateInterval{ mIndex, "Shadows", "v3.3 far cascade update interval",
            makeClampSanitizerInt(1, 8) };
        SettingValue<float> mV33FarCascadeMaxTexelDrift{ mIndex, "Shadows", "v3.3 far cascade max texel drift",
            makeClampSanitizerFloat(0, 8) };
        SettingValue<float> mMinimumLispsmNearFarRatio{ mIndex, "Shadows", "minimum lispsm near far ratio",''',
)

replace_once(
    "files/settings-default.cfg",
    '''v3.2 gpu hard budget mb = 7400
v3.2 streaming max defers = 2

# Preload cells in a background thread.''',
    '''v3.2 gpu hard budget mb = 7400
v3.2 streaming max defers = 2

# V3.3 frame-pacing experiment. 0 preserves the upstream unlimited speculative scheduling behavior.
# Positive values cap only NEW predictive outer-ring cell preload jobs per frame; required cell activation is unchanged.
v3.3 speculative preload budget = 0

# Preload cells in a background thread.''',
)

replace_once(
    "files/settings-default.cfg",
    '''# How large to make the shadow map(s). Higher values increase GPU load, but can produce better-looking results. Power-of-two values may turn out to be faster on some GPU/driver combinations.
shadow map resolution = 1024

# Controls the minimum near/far ratio''',
    '''# How large to make the shadow map(s). Higher values increase GPU load, but can produce better-looking results. Power-of-two values may turn out to be faster on some GPU/driver combinations.
shadow map resolution = 1024

# V3.3 conservative far-cascade reuse. 1 updates every cascade every frame and preserves upstream behavior.
# Values above 1 may reuse only the farthest cascade while its projected drift stays within the texel guard.
# Reuse is automatically disabled when actor or player shadows are enabled.
v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

# Controls the minimum near/far ratio''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''#include <string>
#include <string_view>

namespace Debug::V3HitchTelemetry''',
    '''#include <string>
#include <string_view>

#include "v33framestats.hpp"

namespace Debug::V3HitchTelemetry''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''    constexpr std::size_t StageCount = 10;

    inline std::atomic<unsigned> sCurrentFrame{ 0 };''',
    '''    constexpr std::size_t StageCount = 10;

    enum class FrameTailStage : std::size_t
    {
        PreViewer,
        EventTraversal,
        UpdateTraversal,
        RenderingTraversal,
        LuaWait,
        FrameLimiter,
        ViewerAdvance,
        Count,
    };

    inline std::atomic<unsigned> sCurrentFrame{ 0 };''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            mFrameStart = now;
            mFrameStartSystem = std::chrono::system_clock::now();
            mStageMs.fill(0.0);
        }

        void recordStage(std::size_t stage, double milliseconds)
        {
            if (!mStarted || stage >= mStageMs.size())
                return;
            mStageMs[stage] += milliseconds;
        }''',
    '''            mFrameStart = now;
            mFrameStartSystem = std::chrono::system_clock::now();
            mStageMs.fill(0.0);
            mFrameTailMs.fill(0.0);
        }

        void recordStage(std::size_t stage, double milliseconds)
        {
            if (!mStarted || stage >= mStageMs.size())
                return;
            mStageMs[stage] += milliseconds;
        }

        void recordFrameTail(FrameTailStage stage, double milliseconds)
        {
            const std::size_t index = static_cast<std::size_t>(stage);
            if (!mStarted || index >= mFrameTailMs.size())
                return;
            mFrameTailMs[index] += milliseconds;
        }''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            mAllFrameStream
                << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                   "world_ms,gui_ms,focus_ms,accounted_ms,other_ms\\n";''',
    '''            mAllFrameStream
                << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                   "world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,"
                   "rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms\\n";''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            for (double value : mStageMs)
                mAllFrameStream << ',' << value;
            mAllFrameStream << ',' << accountedMs << ',' << otherMs << '\\n';''',
    '''            for (double value : mStageMs)
                mAllFrameStream << ',' << value;
            for (double value : mFrameTailMs)
                mAllFrameStream << ',' << value;
            mAllFrameStream << ',' << accountedMs << ',' << otherMs << '\\n';''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            mStream << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                       "world_ms,gui_ms,focus_ms,accounted_ms,other_ms,reason\\n";''',
    '''            mStream << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                       "world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,"
                       "rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms,reason\\n";''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            const double otherMs = std::max(0.0, wallMs - accountedMs);
            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());

            emitAllFrame(wallMs, accountedMs, otherMs);''',
    '''            const double tailAccountedMs
                = std::accumulate(mFrameTailMs.begin(), mFrameTailMs.end(), 0.0);
            const double totalAccountedMs = accountedMs + tailAccountedMs;
            const double otherMs = std::max(0.0, wallMs - totalAccountedMs);
            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());
            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();

            V33FrameStats::record(mFrame, epochMs, wallMs);
            emitAllFrame(wallMs, totalAccountedMs, otherMs);''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            const bool slowStage = largestStage >= StageThresholdMs;''',
    '''            const double largestTailStage = *std::max_element(mFrameTailMs.begin(), mFrameTailMs.end());
            const bool slowStage = std::max(largestStage, largestTailStage) >= StageThresholdMs;''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            const char* reason = hitch ? "hitch" : (slowStage ? "slow_stage" : "baseline");
            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();
            mStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mStream << ',' << value;
            mStream << ',' << accountedMs << ',' << otherMs << ',' << reason << '\\n';''',
    '''            const char* reason = hitch ? "hitch" : (slowStage ? "slow_stage" : "baseline");
            mStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mStream << ',' << value;
            for (double value : mFrameTailMs)
                mStream << ',' << value;
            mStream << ',' << totalAccountedMs << ',' << otherMs << ',' << reason << '\\n';''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        std::array<double, StageCount> mStageMs{};
        std::string mPath;''',
    '''        std::array<double, StageCount> mStageMs{};
        std::array<double, static_cast<std::size_t>(FrameTailStage::Count)> mFrameTailMs{};
        std::string mPath;''',
)

replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''    inline void recordStage(std::size_t stage, double milliseconds)
    {
        state().recordStage(stage, milliseconds);
    }
}''',
    '''    inline void recordStage(std::size_t stage, double milliseconds)
    {
        state().recordStage(stage, milliseconds);
    }

    inline void recordFrameTail(FrameTailStage stage, double milliseconds)
    {
        state().recordFrameTail(stage, milliseconds);
    }

    class ScopedFrameTail
    {
    public:
        explicit ScopedFrameTail(FrameTailStage stage)
            : mStage(stage)
            , mStart(std::chrono::steady_clock::now())
        {
        }

        ~ScopedFrameTail()
        {
            recordFrameTail(mStage,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count());
        }

        ScopedFrameTail(const ScopedFrameTail&) = delete;
        ScopedFrameTail& operator=(const ScopedFrameTail&) = delete;

    private:
        FrameTailStage mStage;
        std::chrono::steady_clock::time_point mStart;
    };
}''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    mViewer->eventTraversal();
    mViewer->updateTraversal();''',
    '''    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(Debug::V3HitchTelemetry::FrameTailStage::PreViewer);
        const bool reportResource = stats->collectStats("resource");

        if (reportResource)
            stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

        mUnrefQueue->flush(*mWorkQueue);

        if (reportResource)
        {
            stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

            mResourceSystem->reportStats(frameNumber, stats);

            stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
            stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

            mMechanicsManager->reportStats(frameNumber, *stats);
            mWorld->reportStats(frameNumber, *stats);
            mLuaManager->reportStats(frameNumber, *stats);

            stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
        }

        mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);
    }

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::EventTraversal);
        mViewer->eventTraversal();
    }
    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::UpdateTraversal);
        mViewer->updateTraversal();
    }''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''    mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);''',
    '''    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::RenderingTraversal);
        mViewer->renderingTraversals();
    }

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(Debug::V3HitchTelemetry::FrameTailStage::LuaWait);
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
    }''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();''',
    '''        {
            Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
                Debug::V3HitchTelemetry::FrameTailStage::ViewerAdvance);
            mViewer->advance(timeManager.getRenderingSimulationTime());
        }

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''        frameRateLimiter.limit();''',
    '''        {
            Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
                Debug::V3HitchTelemetry::FrameTailStage::FrameLimiter);
            frameRateLimiter.limit();
        }''',
)

replace_once(
    "components/debug/v3diagnostics.hpp",
    '''        static CsvWriter writer("OPENMW_V32_GPU_MEMORY_FILE",
            "frame,epoch_ms,dedicated_usage_mb,dedicated_budget_mb,available_for_reservation_mb,"
            "current_reservation_mb,budget_used_pct,effective_soft_mb,effective_hard_mb,pressure");''',
    '''        static CsvWriter writer("OPENMW_V32_GPU_MEMORY_FILE",
            "frame,epoch_ms,dedicated_usage_mb,dedicated_budget_mb,available_for_reservation_mb,"
            "current_reservation_mb,budget_used_pct,effective_soft_mb,effective_hard_mb,pressure,"
            "nvml_available,adapter_used_mb,adapter_free_mb,adapter_total_mb");''',
)

replace_once(
    "apps/openmw/mwworld/cellpreloader.hpp",
    '''        std::size_t getCacheSize() const { return mPreloadCells.size(); }

        void setWorkQueue''',
    '''        std::size_t getCacheSize() const { return mPreloadCells.size(); }

        bool isPreloaded(const CellStore& cell) const { return mPreloadCells.find(&cell) != mPreloadCells.end(); }

        void setWorkQueue''',
)

replace_once(
    "apps/openmw/mwworld/scene.hpp",
    '''        bool mPreloadFastTravel;
        float mPredictionTime;''',
    '''        bool mPreloadFastTravel;
        int mV33SpeculativePreloadBudget;
        float mPredictionTime;''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''#include <atomic>
#include <chrono>
#include <limits>''',
    '''#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        , mPreloadDoors(Settings::cells().mPreloadDoors)
        , mPreloadFastTravel(Settings::cells().mPreloadFastTravel)
        , mPredictionTime(Settings::RamCache::predictionTime())''',
    '''        , mPreloadDoors(Settings::cells().mPreloadDoors)
        , mPreloadFastTravel(Settings::cells().mPreloadFastTravel)
        , mV33SpeculativePreloadBudget(Settings::cells().mV33SpeculativePreloadBudget)
        , mPredictionTime(Settings::RamCache::predictionTime())''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_cell_grid", "exterior_grid", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");''',
    '''        Debug::V3Diagnostics::writeEvent("change_cell_grid", "exterior_grid");
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_cell_grid", "exterior_grid", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::preloadExteriorGrid(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        if (!mWorld.isCellExterior())
            return;

        int halfGridSizePlusOne = mHalfGridSize + 1;

        int cellX, cellY;
        cellX = mCurrentGridCenter.x();
        cellY = mCurrentGridCenter.y();
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();

        int cellSize = ESM::getCellSize(extWorldspace);

        for (int dx = -halfGridSizePlusOne; dx <= halfGridSizePlusOne; ++dx)
        {
            for (int dy = -halfGridSizePlusOne; dy <= halfGridSizePlusOne; ++dy)
            {
                if (dy != halfGridSizePlusOne && dy != -halfGridSizePlusOne && dx != halfGridSizePlusOne
                    && dx != -halfGridSizePlusOne)
                    continue; // only care about the outer (not yet loaded) part of the grid
                ESM::ExteriorCellLocation cellIndex(cellX + dx, cellY + dy, extWorldspace);
                const osg::Vec2f thisCellCenter = ESM::indexToPosition(cellIndex, true);

                float dist = std::max(
                    std::abs(thisCellCenter.x() - playerPos.x()), std::abs(thisCellCenter.y() - playerPos.y()));
                dist = std::min(dist,
                    std::max(std::abs(thisCellCenter.x() - predictedPos.x()),
                        std::abs(thisCellCenter.y() - predictedPos.y())));
                float loadDist = cellSize / 2 + cellSize - mCellLoadingThreshold + mPreloadDistance;

                if (dist < loadDist)
                    preloadCell(mWorld.getWorldModel().getExterior(cellIndex));
            }
        }
    }''',
    '''    void Scene::preloadExteriorGrid(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        if (!mWorld.isCellExterior())
            return;

        struct Candidate
        {
            CellStore* mCell = nullptr;
            float mPriority = 0.0f;
        };

        const int halfGridSizePlusOne = mHalfGridSize + 1;
        const int cellX = mCurrentGridCenter.x();
        const int cellY = mCurrentGridCenter.y();
        const ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();
        const int cellSize = ESM::getCellSize(extWorldspace);
        const float loadDist = cellSize / 2 + cellSize - mCellLoadingThreshold + mPreloadDistance;
        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(halfGridSizePlusOne * 8));

        for (int dx = -halfGridSizePlusOne; dx <= halfGridSizePlusOne; ++dx)
        {
            for (int dy = -halfGridSizePlusOne; dy <= halfGridSizePlusOne; ++dy)
            {
                if (dy != halfGridSizePlusOne && dy != -halfGridSizePlusOne && dx != halfGridSizePlusOne
                    && dx != -halfGridSizePlusOne)
                    continue; // only care about the outer (not yet loaded) part of the grid

                const ESM::ExteriorCellLocation cellIndex(cellX + dx, cellY + dy, extWorldspace);
                const osg::Vec2f thisCellCenter = ESM::indexToPosition(cellIndex, true);
                const float playerDist = std::max(
                    std::abs(thisCellCenter.x() - playerPos.x()), std::abs(thisCellCenter.y() - playerPos.y()));
                const float predictedDist = std::max(std::abs(thisCellCenter.x() - predictedPos.x()),
                    std::abs(thisCellCenter.y() - predictedPos.y()));
                if (std::min(playerDist, predictedDist) < loadDist)
                {
                    candidates.push_back(Candidate{ &mWorld.getWorldModel().getExterior(cellIndex),
                        predictedDist + playerDist * 0.25f });
                }
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.mPriority < right.mPriority; });

        unsigned attemptedNew = 0;
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
        }
    }''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''            osg::ref_ptr<osg::Texture2D>        _texture;
            osg::ref_ptr<osg::Camera>           _camera;
        };''',
    '''            osg::ref_ptr<osg::Texture2D>        _texture;
            osg::ref_ptr<osg::Camera>           _camera;
            bool                                _v33HasCachedShadow = false;
            unsigned int                        _v33LastUpdateTraversal = 0;
            osg::Matrixd                        _v33CachedProjection;
            osg::Matrixd                        _v33CachedView;
            osg::Matrixd                        _v33CachedDriftProjection;
            osg::Matrixd                        _v33CachedDriftView;
        };''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        void setWorldMask(unsigned int worldMask) { _worldMask = worldMask; }

        osg::ref_ptr<osg::StateSet> getOrCreateShadowsBinStateSet();''',
    '''        void setWorldMask(unsigned int worldMask) { _worldMask = worldMask; }

        void setV33FarCascadeReuse(unsigned int interval, double maxTexelDrift, bool dynamicActorCasters);

        osg::ref_ptr<osg::StateSet> getOrCreateShadowsBinStateSet();''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        unsigned int                            _worldMask = ~0u;

        class DebugHUD''',
    '''        unsigned int                            _worldMask = ~0u;
        unsigned int                            _v33FarCascadeUpdateInterval = 1;
        double                                  _v33FarCascadeMaxTexelDrift = 0.75;

        class DebugHUD''',
)

replace_once(
    "components/sceneutil/shadow.cpp",
    '''        mShadowSettings->setMultipleShadowMapHint(osgShadow::ShadowSettings::CASCADED);

        if (settings.mEnableDebugHud)''',
    '''        mShadowSettings->setMultipleShadowMapHint(osgShadow::ShadowSettings::CASCADED);

        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift, settings.mActorShadows || settings.mPlayerShadows);

        if (settings.mEnableDebugHud)''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''void SceneUtil::MWShadowTechnique::setShadowFadeStart(float shadowFadeStart)
{
    _shadowFadeStart = shadowFadeStart;
}

void SceneUtil::MWShadowTechnique::enableFrontFaceCulling()''',
    '''void SceneUtil::MWShadowTechnique::setShadowFadeStart(float shadowFadeStart)
{
    _shadowFadeStart = shadowFadeStart;
}

void SceneUtil::MWShadowTechnique::setV33FarCascadeReuse(
    unsigned int interval, double maxTexelDrift, bool dynamicActorCasters)
{
    _v33FarCascadeUpdateInterval = dynamicActorCasters ? 1u : std::max(1u, interval);
    _v33FarCascadeMaxTexelDrift = std::max(0.0, maxTexelDrift);
}

void SceneUtil::MWShadowTechnique::enableFrontFaceCulling()''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''        "cascade4_ms,cascade5_ms,cascade6_ms,cascade7_ms,num_cascades");
    const bool v3ShadowProfile = v3ShadowWriter.enabled();''',
    '''        "cascade4_ms,cascade5_ms,cascade6_ms,cascade7_ms,num_cascades,updated_cascades,reused_cascades,"
        "max_reuse_texel_drift");
    const bool v3ShadowProfile = v3ShadowWriter.enabled();''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    std::array<double, 8> v3CascadeMs{};
    unsigned int v3CascadeCount = 0;

    if (!_shadowCastingStateSet)''',
    '''    std::array<double, 8> v3CascadeMs{};
    unsigned int v3CascadeCount = 0;
    unsigned int v33UpdatedCascades = 0;
    unsigned int v33ReusedCascades = 0;
    double v33MaxReuseTexelDrift = 0.0;

    if (!_shadowCastingStateSet)''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''            osg::ref_ptr<VDSMCameraCullCallback> vdsmCallback = new VDSMCameraCullCallback(this, local_polytope);
            camera->setCullCallback(vdsmCallback.get());

            // 4.3 traverse RTT camera
            //

            cv.pushStateSet(_shadowCastingStateSet.get());

            const auto v3CasterStart = v3ShadowProfile ? Debug::V3Diagnostics::Clock::now()
                                                             : Debug::V3Diagnostics::Clock::time_point{};
            cullShadowCastingScene(&cv, camera.get());
            if (v3ShadowProfile)
            {
                const double cascadeMs = Debug::V3Diagnostics::elapsedMs(v3CasterStart);
                v3CasterTotalMs += cascadeMs;
                if (v3CascadeCount < v3CascadeMs.size())
                    v3CascadeMs[v3CascadeCount] = cascadeMs;
                ++v3CascadeCount;
            }

            cv.popStateSet();

            if (!orthographicViewFrustum && settings->getShadowMapProjectionHint()==ShadowSettings::PERSPECTIVE_SHADOW_MAP)
            {
                assignValidRegionSettings(cv, camera, sm_i, vddUniforms);

                if (settings->getMultipleShadowMapHint() == ShadowSettings::CASCADED)
                    adjustPerspectiveShadowMapCameraSettings(vdsmCallback->getRenderStage(), frustum, pl, camera.get(), cascaseNear, cascadeFar);
                else
                    adjustPerspectiveShadowMapCameraSettings(vdsmCallback->getRenderStage(), frustum, pl, camera.get(), reducedNear, reducedFar);
                if (vdsmCallback->getProjectionMatrix())
                {
                    vdsmCallback->getProjectionMatrix()->set(camera->getProjectionMatrix());
                }
            }''',
    '''            bool v33ReuseFarCascade = false;
            double v33ReuseTexelDrift = 0.0;
            const unsigned int traversalNumber = cv.getTraversalNumber();
            const osg::Matrixd v33CandidateProjection = camera->getProjectionMatrix();
            const osg::Matrixd v33CandidateView = camera->getViewMatrix();
            if (_v33FarCascadeUpdateInterval > 1 && sm_i + 1 == numShadowMapsPerLight
                && sd->_v33HasCachedShadow
                && sd->_sm_i == sm_i && sd->_textureUnit == textureUnit
                && traversalNumber - sd->_v33LastUpdateTraversal < _v33FarCascadeUpdateInterval)
            {
                const osg::Matrixd candidateViewProjection
                    = v33CandidateView * v33CandidateProjection;
                const osg::Matrixd cachedViewProjection
                    = sd->_v33CachedDriftView * sd->_v33CachedDriftProjection;
                const double texelScale = static_cast<double>(settings->getTextureSize().x()) * 0.5;
                for (const osg::Vec3d& corner : frustum.corners)
                {
                    const osg::Vec3d candidate = corner * candidateViewProjection;
                    const osg::Vec3d cached = corner * cachedViewProjection;
                    v33ReuseTexelDrift = std::max(v33ReuseTexelDrift,
                        std::max(std::abs(candidate.x() - cached.x()), std::abs(candidate.y() - cached.y()))
                            * texelScale);
                }
                v33ReuseFarCascade = v33ReuseTexelDrift <= _v33FarCascadeMaxTexelDrift;
            }

            osg::ref_ptr<VDSMCameraCullCallback> vdsmCallback;
            if (v33ReuseFarCascade)
            {
                camera->setProjectionMatrix(sd->_v33CachedProjection);
                camera->setViewMatrix(sd->_v33CachedView);
                ++v33ReusedCascades;
                v33MaxReuseTexelDrift = std::max(v33MaxReuseTexelDrift, v33ReuseTexelDrift);
            }
            else
            {
                vdsmCallback = new VDSMCameraCullCallback(this, local_polytope);
                camera->setCullCallback(vdsmCallback.get());

                // 4.3 traverse RTT camera
                cv.pushStateSet(_shadowCastingStateSet.get());
                const auto v3CasterStart = v3ShadowProfile ? Debug::V3Diagnostics::Clock::now()
                                                           : Debug::V3Diagnostics::Clock::time_point{};
                cullShadowCastingScene(&cv, camera.get());
                if (v3ShadowProfile)
                {
                    const double cascadeMs = Debug::V3Diagnostics::elapsedMs(v3CasterStart);
                    v3CasterTotalMs += cascadeMs;
                    if (v3CascadeCount < v3CascadeMs.size())
                        v3CascadeMs[v3CascadeCount] = cascadeMs;
                }
                cv.popStateSet();
                ++v33UpdatedCascades;
            }
            if (v3ShadowProfile)
                ++v3CascadeCount;

            if (!orthographicViewFrustum && settings->getShadowMapProjectionHint()==ShadowSettings::PERSPECTIVE_SHADOW_MAP)
            {
                assignValidRegionSettings(cv, camera, sm_i, vddUniforms);

                if (!v33ReuseFarCascade)
                {
                    if (settings->getMultipleShadowMapHint() == ShadowSettings::CASCADED)
                        adjustPerspectiveShadowMapCameraSettings(vdsmCallback->getRenderStage(), frustum, pl,
                            camera.get(), cascaseNear, cascadeFar);
                    else
                        adjustPerspectiveShadowMapCameraSettings(vdsmCallback->getRenderStage(), frustum, pl,
                            camera.get(), reducedNear, reducedFar);
                    if (vdsmCallback->getProjectionMatrix())
                        vdsmCallback->getProjectionMatrix()->set(camera->getProjectionMatrix());
                }
            }

            if (!v33ReuseFarCascade)
            {
                sd->_v33CachedProjection = camera->getProjectionMatrix();
                sd->_v33CachedView = camera->getViewMatrix();
                sd->_v33CachedDriftProjection = v33CandidateProjection;
                sd->_v33CachedDriftView = v33CandidateView;
                sd->_v33LastUpdateTraversal = traversalNumber;
                sd->_v33HasCachedShadow = true;
            }''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''        row << ',' << v3CascadeCount;
        v3ShadowWriter.writeLine(row.str());''',
    '''        row << ',' << v3CascadeCount << ',' << v33UpdatedCascades << ',' << v33ReusedCascades << ','
            << v33MaxReuseTexelDrift;
        v3ShadowWriter.writeLine(row.str());''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$Prepared = 'false'
$Scheduler = 'off'
$RendererProfiling = if ($Mode -eq 'Transition') { 'true' } else { 'false' }
if ($Mode -ne 'Render') {
    Write-Host ''
    Write-Host 'Choose the runtime experiment for this test:' -ForegroundColor Cyan
    Write-Host '  1 = Baseline Overdrive (all runtime experiments off)'
    Write-Host '  2 = V3.2 recent-exterior hibernation'
    Write-Host '  3 = V3.2 Adaptive Scheduler v2'
    Write-Host '  4 = Hibernation + Adaptive v2'
    Write-Host '  5 = Legacy Prepared Static Instances v1'
    Write-Host '  6 = Legacy Adaptive Scheduler v1'
    Write-Host '  7 = Legacy Prepared v1 + Adaptive v1'
    Write-Host '  8 = Hibernation + Adaptive v2 + Prepared v1'
    do { $choice = Read-Host 'Enter 1 through 8' } until ($choice -in @('1','2','3','4','5','6','7','8'))
    switch ($choice) {
        '1' { $Experiment = 'baseline'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'off' }
        '2' { $Experiment = 'hibernation'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'off' }
        '3' { $Experiment = 'adaptive-v2'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
        '4' { $Experiment = 'hibernation-adaptive-v2'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
        '5' { $Experiment = 'prepared-v1'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'off' }
        '6' { $Experiment = 'adaptive-v1'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive' }
        '7' { $Experiment = 'legacy-combined'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'adaptive' }
        '8' { $Experiment = 'all-experimental'; $Hibernation = 'true'; $Prepared = 'true'; $Scheduler = 'adaptive-v2' }
    }
}''',
    '''$Prepared = 'false'
$Scheduler = 'off'
$PreloadBudget = '0'
$FarShadowInterval = '1'
$FarShadowMaxTexelDrift = '0.75'
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }
Write-Host ''
Write-Host 'Choose the runtime experiment for this test:' -ForegroundColor Cyan
Write-Host '  1 = Baseline Overdrive (all runtime experiments off)'
Write-Host '  2 = V3.2 recent-exterior hibernation'
Write-Host '  3 = V3.2 Adaptive Scheduler v2'
Write-Host '  4 = Hibernation + Adaptive v2'
Write-Host '  5 = Legacy Prepared Static Instances v1'
Write-Host '  6 = Legacy Adaptive Scheduler v1'
Write-Host '  7 = Legacy Prepared v1 + Adaptive v1'
Write-Host '  8 = Hibernation + Adaptive v2 + Prepared v1'
Write-Host '  9 = V3.3 predictive preload budget (diagnostic; budget 2 rarely binds)'
Write-Host ' 10 = V3.3 far-shadow reuse (requires actor and player shadows off)'
Write-Host ' 11 = V3.3 combined legacy experiments (same limitations as 9 and 10)'
do { $choice = Read-Host 'Enter 1 through 11' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11'))
switch ($choice) {
    '1' { $Experiment = 'baseline'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'off' }
    '2' { $Experiment = 'hibernation'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'off' }
    '3' { $Experiment = 'adaptive-v2'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
    '4' { $Experiment = 'hibernation-adaptive-v2'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
    '5' { $Experiment = 'prepared-v1'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'off' }
    '6' { $Experiment = 'adaptive-v1'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive' }
    '7' { $Experiment = 'legacy-combined'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'adaptive' }
    '8' { $Experiment = 'all-experimental'; $Hibernation = 'true'; $Prepared = 'true'; $Scheduler = 'adaptive-v2' }
    '9' { $Experiment = 'v33-preload-budget'; $PreloadBudget = '2' }
    '10' { $Experiment = 'v33-far-shadow-reuse'; $FarShadowInterval = '2' }
    '11' { $Experiment = 'v33-framepacing-gpu'; $PreloadBudget = '2'; $FarShadowInterval = '2' }
}''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "openmw_exe_sha256=$exeHash",
    "game_dir=$GameDir"''',
    '''    "openmw_exe_sha256=$exeHash",
    "game_dir=$GameDir",
    "v33_speculative_preload_budget=$PreloadBudget",
    "v33_far_shadow_update_interval=$FarShadowInterval",
    "v33_far_shadow_max_texel_drift=$FarShadowMaxTexelDrift"''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
    '''    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    """$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'
$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'""",
    """$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'
$env:OPENMW_V33_FRAME_SUMMARY_FILE = Join-Path $ProfileDir 'v33-frame-summary.csv'
$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'""",
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''if ($Mode -eq 'City') {
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
}''',
    '''if ($Mode -eq 'City') {
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_LUASYNC_FILE = Join-Path $ProfileDir 'v3-luasync.csv'
    $env:OPENMW_V3_LUA_ACTION_FILE = Join-Path $ProfileDir 'v3-lua-actions.csv'
    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RESOURCE_FILE = Join-Path $ProfileDir 'v3-resource.csv'
    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V32_RENDER_INSERT_FILE = Join-Path $ProfileDir 'v3-render-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
    $env:OPENMW_V3_SHADOW_FILE = Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Cells' 'v3.2 streaming max defers' '2'
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
    '''    Set-IniValue $SettingsPath 'Cells' 'v3.2 streaming max defers' '2'
    Set-IniValue $SettingsPath 'Cells' 'v3.3 speculative preload budget' $PreloadBudget
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade update interval' $FarShadowInterval
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade max texel drift' $FarShadowMaxTexelDrift
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
)

print("V3.3 frame-pacing, shadow reuse, speculative admission, and telemetry patch completed successfully.")
