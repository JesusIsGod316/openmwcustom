#ifndef OPENMW_COMPONENTS_DEBUG_V33LUATRACE_H
#define OPENMW_COMPONENTS_DEBUG_V33LUATRACE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"

namespace Debug::V33LuaTrace
{
    using Clock = V3Diagnostics::Clock;

    enum class Phase : std::size_t
    {
        None,
        Timers,
        LuaEvents,
        QueuedCallbacks,
        EngineEvents,
        LocalUpdate,
        GlobalUpdate,
        Count,
    };

    inline constexpr std::size_t PhaseCount = static_cast<std::size_t>(Phase::Count);

    inline std::string_view phaseName(Phase phase)
    {
        switch (phase)
        {
            case Phase::Timers:
                return "timers";
            case Phase::LuaEvents:
                return "lua_events";
            case Phase::QueuedCallbacks:
                return "queued_callbacks";
            case Phase::EngineEvents:
                return "engine_events";
            case Phase::LocalUpdate:
                return "local_update";
            case Phase::GlobalUpdate:
                return "global_update";
            case Phase::None:
            case Phase::Count:
                return "none";
        }
        return "none";
    }

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V33_LUA_CALLBACK_FILE",
            "frame,epoch_ms,row_kind,phase,context,scope,container,object_ref,script_id,script_path,"
            "callback_name,count,elapsed_ms,max_ms,ordinal,detail");
        return writer;
    }

    struct Sample
    {
        Phase mPhase = Phase::None;
        std::string mContext;
        std::string mScope;
        std::string mContainer;
        std::string mObjectRef;
        int mScriptId = -1;
        std::string mScriptPath;
        std::string mCallbackName;
        std::string mDetail;
        double mElapsedMs = 0.0;
        unsigned mOrdinal = 0;
    };

    struct EngineEventSummary
    {
        std::string mName;
        unsigned mCount = 0;
        double mTotalMs = 0.0;
        double mMaxMs = 0.0;
    };

    class Recorder
    {
    public:
        static constexpr std::size_t MaxSamples = 8;
        static constexpr std::size_t MaxEngineEventTypes = 16;
        static constexpr double SlowPhaseThresholdMs = 8.0;

        void begin()
        {
            mFrame = V3HitchTelemetry::currentFrame();
            mEpochMs = V3Diagnostics::epochMs();
            mFrameStart = Clock::now();
        }

        Phase phase() const { return mPhase; }
        void setPhase(Phase phase) { mPhase = phase; }

        void addPhaseTime(Phase phase, double elapsedMs)
        {
            const std::size_t index = static_cast<std::size_t>(phase);
            if (index < mPhaseMs.size())
                mPhaseMs[index] += elapsedMs;
        }

        void recordSample(Phase phase, std::string_view context, std::string_view scope,
            std::string_view container, int scriptId, std::string_view scriptPath, std::string_view callbackName,
            std::string_view detail, double elapsedMs)
        {
            const std::size_t phaseIndex = static_cast<std::size_t>(phase);
            if (phaseIndex < mCallbackCounts.size())
                ++mCallbackCounts[phaseIndex];

            std::size_t target = mSampleCount;
            if (mSampleCount < MaxSamples)
                ++mSampleCount;
            else
            {
                target = 0;
                for (std::size_t i = 1; i < MaxSamples; ++i)
                {
                    if (mSamples[i].mElapsedMs < mSamples[target].mElapsedMs)
                        target = i;
                }
                if (elapsedMs <= mSamples[target].mElapsedMs)
                    return;
            }

            Sample& sample = mSamples[target];
            sample.mPhase = phase;
            sample.mContext.assign(context);
            sample.mScope.assign(scope);
            sample.mContainer.assign(container);
            sample.mObjectRef = !container.empty() && container.front() == 'L' ? std::string(container.substr(1)) : "";
            sample.mScriptId = scriptId;
            sample.mScriptPath.assign(scriptPath);
            sample.mCallbackName.assign(callbackName);
            sample.mDetail.assign(detail);
            sample.mElapsedMs = elapsedMs;
            sample.mOrdinal = ++mSampleOrdinal;
        }

        void recordTimerContainer(bool due, bool fastSkipped, unsigned simulationFired, unsigned gameFired)
        {
            ++mTimerContainersExamined;
            mTimerContainersDue += due;
            mTimerContainersFastSkipped += fastSkipped;
            mSimulationTimersFired += simulationFired;
            mGameTimersFired += gameFired;
        }

        void recordEngineEvent(std::string_view name, double elapsedMs)
        {
            EngineEventSummary* target = nullptr;
            for (std::size_t i = 0; i < mEngineEventTypeCount; ++i)
            {
                if (mEngineEventTypes[i].mName == name)
                {
                    target = &mEngineEventTypes[i];
                    break;
                }
            }
            if (!target && mEngineEventTypeCount < MaxEngineEventTypes)
            {
                target = &mEngineEventTypes[mEngineEventTypeCount++];
                target->mName.assign(name);
            }
            if (!target)
                return;
            ++target->mCount;
            target->mTotalMs += elapsedMs;
            target->mMaxMs = std::max(target->mMaxMs, elapsedMs);
        }

        void recordEngineEventBatch(unsigned queueSize, unsigned uniqueObjects, unsigned duplicateSameTypeObjects,
            unsigned activeInactiveSameFrameObjects)
        {
            mEngineQueueSize = queueSize;
            mEngineUniqueObjects = uniqueObjects;
            mEngineDuplicateSameTypeObjects = duplicateSameTypeObjects;
            mEngineActiveInactiveSameFrameObjects = activeInactiveSameFrameObjects;
        }

        void finish()
        {
            const double totalMs = V3Diagnostics::elapsedMs(mFrameStart);
            double slowestPhaseMs = 0.0;
            for (std::size_t i = 1; i < PhaseCount; ++i)
                slowestPhaseMs = std::max(slowestPhaseMs, mPhaseMs[i]);
            if (slowestPhaseMs < SlowPhaseThresholdMs)
                return;

            for (std::size_t i = 1; i < PhaseCount; ++i)
            {
                if (mPhaseMs[i] < SlowPhaseThresholdMs)
                    continue;
                std::ostringstream detail;
                detail << "lua_update_ms=" << std::fixed << std::setprecision(3) << totalMs;
                writeRow("phase_summary", static_cast<Phase>(i), "phase", "", "", "", -1, "", "",
                    mCallbackCounts[i], mPhaseMs[i], mPhaseMs[i], 0, detail.str());
            }

            const std::size_t timerIndex = static_cast<std::size_t>(Phase::Timers);
            if (mPhaseMs[timerIndex] >= SlowPhaseThresholdMs)
            {
                std::ostringstream detail;
                detail << "containers_examined=" << mTimerContainersExamined << ";containers_due="
                       << mTimerContainersDue << ";fast_skipped=" << mTimerContainersFastSkipped
                       << ";simulation_fired=" << mSimulationTimersFired << ";game_fired=" << mGameTimersFired;
                writeRow("timer_summary", Phase::Timers, "timer", "all", "", "", -1, "", "",
                    mSimulationTimersFired + mGameTimersFired, mPhaseMs[timerIndex], 0.0, 0, detail.str());
            }

            const std::size_t engineIndex = static_cast<std::size_t>(Phase::EngineEvents);
            if (mPhaseMs[engineIndex] >= SlowPhaseThresholdMs)
            {
                std::ostringstream detail;
                detail << "unique_objects=" << mEngineUniqueObjects << ";duplicate_same_type_objects="
                       << mEngineDuplicateSameTypeObjects << ";active_inactive_same_frame_objects="
                       << mEngineActiveInactiveSameFrameObjects;
                writeRow("engine_batch_summary", Phase::EngineEvents, "engine_event", "batch", "", "", -1,
                    "", "", mEngineQueueSize, mPhaseMs[engineIndex], 0.0, 0, detail.str());
                for (std::size_t i = 0; i < mEngineEventTypeCount; ++i)
                {
                    const EngineEventSummary& event = mEngineEventTypes[i];
                    writeRow("engine_type_summary", Phase::EngineEvents, "engine_event", event.mName, "", "",
                        -1, "", event.mName, event.mCount, event.mTotalMs, event.mMaxMs, 0, "");
                }
            }

            std::sort(mSamples.begin(), mSamples.begin() + mSampleCount,
                [](const Sample& left, const Sample& right) { return left.mElapsedMs > right.mElapsedMs; });
            unsigned rank = 0;
            for (std::size_t i = 0; i < mSampleCount; ++i)
            {
                const Sample& sample = mSamples[i];
                if (mPhaseMs[static_cast<std::size_t>(sample.mPhase)] < SlowPhaseThresholdMs)
                    continue;
                writeRow("callback", sample.mPhase, sample.mContext, sample.mScope, sample.mContainer,
                    sample.mObjectRef, sample.mScriptId, sample.mScriptPath, sample.mCallbackName, 1,
                    sample.mElapsedMs, sample.mElapsedMs, ++rank, sample.mDetail);
            }
        }

    private:
        void writeRow(std::string_view rowKind, Phase phase, std::string_view context, std::string_view scope,
            std::string_view container, std::string_view objectRef, int scriptId, std::string_view scriptPath,
            std::string_view callbackName, unsigned count, double elapsedMs, double maxMs, unsigned ordinal,
            std::string_view detail)
        {
            std::ostringstream row;
            row << mFrame << ',' << mEpochMs << ',' << V3Diagnostics::csvQuote(rowKind) << ','
                << V3Diagnostics::csvQuote(phaseName(phase)) << ',' << V3Diagnostics::csvQuote(context) << ','
                << V3Diagnostics::csvQuote(scope) << ',' << V3Diagnostics::csvQuote(container) << ','
                << V3Diagnostics::csvQuote(objectRef) << ',' << scriptId << ','
                << V3Diagnostics::csvQuote(scriptPath) << ',' << V3Diagnostics::csvQuote(callbackName) << ','
                << count << ',' << std::fixed << std::setprecision(3) << elapsedMs << ',' << maxMs << ','
                << ordinal << ',' << V3Diagnostics::csvQuote(detail);
            writer().writeLine(row.str());
        }

        unsigned mFrame = 0;
        long long mEpochMs = 0;
        Clock::time_point mFrameStart{};
        Phase mPhase = Phase::None;
        std::array<double, PhaseCount> mPhaseMs{};
        std::array<unsigned, PhaseCount> mCallbackCounts{};
        std::array<Sample, MaxSamples> mSamples{};
        std::size_t mSampleCount = 0;
        unsigned mSampleOrdinal = 0;
        unsigned mTimerContainersExamined = 0;
        unsigned mTimerContainersDue = 0;
        unsigned mTimerContainersFastSkipped = 0;
        unsigned mSimulationTimersFired = 0;
        unsigned mGameTimersFired = 0;
        std::array<EngineEventSummary, MaxEngineEventTypes> mEngineEventTypes{};
        std::size_t mEngineEventTypeCount = 0;
        unsigned mEngineQueueSize = 0;
        unsigned mEngineUniqueObjects = 0;
        unsigned mEngineDuplicateSameTypeObjects = 0;
        unsigned mEngineActiveInactiveSameFrameObjects = 0;
    };

    inline thread_local Recorder* sRecorder = nullptr;

    class FrameScope
    {
    public:
        FrameScope()
        {
            if (sRecorder || !writer().enabled())
                return;
            mActive = true;
            mPrevious = sRecorder;
            sRecorder = &mRecorder;
            mRecorder.begin();
        }

        ~FrameScope()
        {
            if (!mActive)
                return;
            mRecorder.finish();
            sRecorder = mPrevious;
        }

        FrameScope(const FrameScope&) = delete;
        FrameScope& operator=(const FrameScope&) = delete;

    private:
        Recorder mRecorder;
        Recorder* mPrevious = nullptr;
        bool mActive = false;
    };

    class PhaseScope
    {
    public:
        explicit PhaseScope(Phase phase)
            : mRecorder(sRecorder)
            , mPhase(phase)
        {
            if (!mRecorder)
                return;
            mPrevious = mRecorder->phase();
            mRecorder->setPhase(phase);
            mStart = Clock::now();
        }

        ~PhaseScope()
        {
            if (!mRecorder)
                return;
            mRecorder->addPhaseTime(mPhase, V3Diagnostics::elapsedMs(mStart));
            mRecorder->setPhase(mPrevious);
        }

        PhaseScope(const PhaseScope&) = delete;
        PhaseScope& operator=(const PhaseScope&) = delete;

    private:
        Recorder* mRecorder = nullptr;
        Phase mPhase = Phase::None;
        Phase mPrevious = Phase::None;
        Clock::time_point mStart{};
    };

    class CallbackScope
    {
    public:
        CallbackScope(std::string_view context, std::string_view scope, std::string_view container, int scriptId,
            std::string_view scriptPath, std::string_view callbackName, std::string_view detail = {})
            : mRecorder(sRecorder)
            , mPhase(mRecorder ? mRecorder->phase() : Phase::None)
            , mContext(context)
            , mScope(scope)
            , mContainer(container)
            , mScriptId(scriptId)
            , mScriptPath(scriptPath)
            , mCallbackName(callbackName)
            , mDetail(detail)
            , mStart(mRecorder ? Clock::now() : Clock::time_point{})
        {
        }

        ~CallbackScope()
        {
            if (mRecorder)
                mRecorder->recordSample(mPhase, mContext, mScope, mContainer, mScriptId, mScriptPath,
                    mCallbackName, mDetail, V3Diagnostics::elapsedMs(mStart));
        }

        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;

    private:
        Recorder* mRecorder;
        Phase mPhase;
        std::string_view mContext;
        std::string_view mScope;
        std::string_view mContainer;
        int mScriptId;
        std::string_view mScriptPath;
        std::string_view mCallbackName;
        std::string_view mDetail;
        Clock::time_point mStart;
    };

    class EngineEventScope
    {
    public:
        explicit EngineEventScope(std::string_view eventName)
            : mRecorder(sRecorder)
            , mEventName(eventName)
            , mStart(mRecorder ? Clock::now() : Clock::time_point{})
        {
        }

        ~EngineEventScope()
        {
            if (!mRecorder)
                return;
            const double elapsedMs = V3Diagnostics::elapsedMs(mStart);
            mRecorder->recordEngineEvent(mEventName, elapsedMs);
            mRecorder->recordSample(Phase::EngineEvents, "engine_event", mEventName, "", -1, "", mEventName,
                "", elapsedMs);
        }

        EngineEventScope(const EngineEventScope&) = delete;
        EngineEventScope& operator=(const EngineEventScope&) = delete;

    private:
        Recorder* mRecorder;
        std::string_view mEventName;
        Clock::time_point mStart;
    };

    inline void recordTimerContainer(bool due, bool fastSkipped, unsigned simulationFired, unsigned gameFired)
    {
        if (sRecorder)
            sRecorder->recordTimerContainer(due, fastSkipped, simulationFired, gameFired);
    }

    inline bool enabled() { return sRecorder != nullptr; }

    inline void recordEngineEventBatch(unsigned queueSize, unsigned uniqueObjects,
        unsigned duplicateSameTypeObjects, unsigned activeInactiveSameFrameObjects)
    {
        if (sRecorder)
        {
            sRecorder->recordEngineEventBatch(
                queueSize, uniqueObjects, duplicateSameTypeObjects, activeInactiveSameFrameObjects);
        }
    }
}

#endif
