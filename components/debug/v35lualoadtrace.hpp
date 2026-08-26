#ifndef OPENMW_COMPONENTS_DEBUG_V35LUALOADTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V35LUALOADTRACE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V35LuaLoadTrace
{
    using Clock = V3Diagnostics::Clock;

    enum class Phase : std::size_t
    {
        Prepare,
        Interfaces,
        AddScripts,
        InitLoad,
        Timers,
        Heap,
        Tracker,
        Count,
    };

    inline constexpr std::size_t PhaseCount = static_cast<std::size_t>(Phase::Count);

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V35_LUA_LOAD_FILE",
            "frame,epoch_ms,container,total_ms,prepare_ms,interfaces_ms,add_scripts_ms,init_load_ms,timers_ms,"
            "heap_ms,tracker_ms,script_count,saved_script_count,timer_count,max_script_ms,max_script_path");
        return writer;
    }

    class Recorder
    {
    public:
        void begin(std::string_view container)
        {
            mContainer.assign(container);
            mStart = Clock::now();
        }

        void addPhase(Phase phase, double milliseconds)
        {
            const std::size_t index = static_cast<std::size_t>(phase);
            if (index < mPhaseMs.size())
                mPhaseMs[index] += milliseconds;
        }

        void setScriptCounts(unsigned scripts, unsigned savedScripts)
        {
            mScriptCount = scripts;
            mSavedScriptCount = savedScripts;
        }

        void addTimerCount(unsigned timers) { mTimerCount += timers; }

        void recordScript(std::string_view path, double milliseconds)
        {
            if (milliseconds <= mMaxScriptMs)
                return;
            mMaxScriptMs = milliseconds;
            mMaxScriptPath.assign(path);
        }

        void finish()
        {
            const double totalMs = V3Diagnostics::elapsedMs(mStart);
            // Keep enough detail to explain accumulation across hundreds of containers while
            // avoiding a row for every trivially cheap first materialization.
            if (totalMs < 2.0)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
                << V3Diagnostics::csvQuote(mContainer) << ',' << std::fixed << std::setprecision(3) << totalMs;
            for (double milliseconds : mPhaseMs)
                row << ',' << milliseconds;
            row << ',' << mScriptCount << ',' << mSavedScriptCount << ',' << mTimerCount << ',' << mMaxScriptMs << ','
                << V3Diagnostics::csvQuote(mMaxScriptPath);
            writer().writeLine(row.str());
        }

    private:
        std::string mContainer;
        Clock::time_point mStart{};
        std::array<double, PhaseCount> mPhaseMs{};
        unsigned mScriptCount = 0;
        unsigned mSavedScriptCount = 0;
        unsigned mTimerCount = 0;
        double mMaxScriptMs = 0.0;
        std::string mMaxScriptPath;
    };

    inline thread_local Recorder* sRecorder = nullptr;

    class LoadScope
    {
    public:
        explicit LoadScope(std::string_view container)
        {
            if (sRecorder || !writer().enabled())
                return;
            mActive = true;
            mPrevious = sRecorder;
            sRecorder = &mRecorder;
            mRecorder.begin(container);
        }

        ~LoadScope()
        {
            if (!mActive)
                return;
            mRecorder.finish();
            sRecorder = mPrevious;
        }

        LoadScope(const LoadScope&) = delete;
        LoadScope& operator=(const LoadScope&) = delete;

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
            , mStart(mRecorder ? Clock::now() : Clock::time_point{})
        {
        }

        ~PhaseScope()
        {
            if (mRecorder)
                mRecorder->addPhase(mPhase, V3Diagnostics::elapsedMs(mStart));
        }

        PhaseScope(const PhaseScope&) = delete;
        PhaseScope& operator=(const PhaseScope&) = delete;

    private:
        Recorder* mRecorder;
        Phase mPhase;
        Clock::time_point mStart;
    };

    class ScriptScope
    {
    public:
        explicit ScriptScope(std::string_view path)
            : mRecorder(sRecorder)
            , mPath(path)
            , mStart(mRecorder ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScriptScope()
        {
            if (mRecorder)
                mRecorder->recordScript(mPath, V3Diagnostics::elapsedMs(mStart));
        }

        ScriptScope(const ScriptScope&) = delete;
        ScriptScope& operator=(const ScriptScope&) = delete;

    private:
        Recorder* mRecorder;
        std::string_view mPath;
        Clock::time_point mStart;
    };

    inline void setScriptCounts(unsigned scripts, unsigned savedScripts)
    {
        if (sRecorder)
            sRecorder->setScriptCounts(scripts, savedScripts);
    }

    inline void addTimerCount(unsigned timers = 1)
    {
        if (sRecorder)
            sRecorder->addTimerCount(timers);
    }
}

#endif
