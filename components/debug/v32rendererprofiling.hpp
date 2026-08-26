#ifndef OPENMW_COMPONENTS_DEBUG_V32RENDERERPROFILING_H
#define OPENMW_COMPONENTS_DEBUG_V32RENDERERPROFILING_H

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace Debug::V32RendererProfiling
{
    using Clock = std::chrono::steady_clock;

    enum class Phase
    {
        SceneInstance,
        ObjectRoot,
        ControllerSetup,
        TransformAttach,
    };

    struct Stats
    {
        std::size_t mObjects = 0;
        std::size_t mConstructed = 0;
        std::size_t mRestored = 0;
        std::size_t mPaged = 0;
        std::size_t mStatic = 0;
        std::size_t mAnimated = 0;
        std::size_t mActors = 0;
        std::size_t mLights = 0;
        double mRendererTotalMs = 0.0;
        double mSceneInstanceMs = 0.0;
        double mObjectRootMs = 0.0;
        double mControllerSetupMs = 0.0;
        double mTransformAttachMs = 0.0;
        double mMaxObjectMs = 0.0;
        std::string mMaxRef;
        std::string mMaxModel;
    };

    inline thread_local Stats* sActiveStats = nullptr;

    inline double elapsedMs(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    class Scope
    {
    public:
        explicit Scope(Stats* stats)
            : mPrevious(sActiveStats)
        {
            sActiveStats = stats;
        }

        ~Scope() { sActiveStats = mPrevious; }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Stats* mPrevious;
    };

    class ScopedPhase
    {
    public:
        explicit ScopedPhase(Phase phase)
            : mStats(sActiveStats)
            , mPhase(phase)
        {
            if (mStats)
                mStart = Clock::now();
        }

        ~ScopedPhase()
        {
            if (!mStats)
                return;

            const double durationMs = elapsedMs(mStart);
            switch (mPhase)
            {
                case Phase::SceneInstance:
                    mStats->mSceneInstanceMs += durationMs;
                    break;
                case Phase::ObjectRoot:
                    mStats->mObjectRootMs += durationMs;
                    break;
                case Phase::ControllerSetup:
                    mStats->mControllerSetupMs += durationMs;
                    break;
                case Phase::TransformAttach:
                    mStats->mTransformAttachMs += durationMs;
                    break;
            }
        }

        ScopedPhase(const ScopedPhase&) = delete;
        ScopedPhase& operator=(const ScopedPhase&) = delete;

    private:
        Stats* mStats;
        Phase mPhase;
        Clock::time_point mStart{};
    };

    inline Clock::time_point beginObject()
    {
        return sActiveStats ? Clock::now() : Clock::time_point{};
    }

    inline void finishObject(Clock::time_point start, bool constructed, bool restored, bool paged, bool actor,
        bool animated, bool light, std::string_view refId, std::string_view model)
    {
        Stats* stats = sActiveStats;
        if (!stats || start == Clock::time_point{})
            return;

        const double durationMs = elapsedMs(start);
        ++stats->mObjects;
        if (constructed)
            ++stats->mConstructed;
        if (restored)
            ++stats->mRestored;
        if (paged)
            ++stats->mPaged;
        if (actor)
            ++stats->mActors;
        else if (animated)
            ++stats->mAnimated;
        else
            ++stats->mStatic;
        if (light)
            ++stats->mLights;

        stats->mRendererTotalMs += durationMs;
        if (durationMs > stats->mMaxObjectMs)
        {
            stats->mMaxObjectMs = durationMs;
            stats->mMaxRef.assign(refId);
            stats->mMaxModel.assign(model);
        }
    }
}

#endif
