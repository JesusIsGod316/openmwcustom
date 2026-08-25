#ifndef OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H
#define OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "v3hitchtelemetry.hpp"

namespace Debug::V3Diagnostics
{
    using Clock = std::chrono::steady_clock;

    inline double elapsedMs(Clock::time_point start, Clock::time_point end = Clock::now())
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    inline long long epochMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    inline std::size_t threadId()
    {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
    }

    class ScopedAccumulator
    {
    public:
        ScopedAccumulator(bool enabled, double& accumulator)
            : mEnabled(enabled)
            , mAccumulator(accumulator)
            , mStart(enabled ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScopedAccumulator()
        {
            if (mEnabled)
                mAccumulator += elapsedMs(mStart);
        }

        ScopedAccumulator(const ScopedAccumulator&) = delete;
        ScopedAccumulator& operator=(const ScopedAccumulator&) = delete;

    private:
        bool mEnabled;
        double& mAccumulator;
        Clock::time_point mStart;
    };

    inline bool pathEnabled(std::string_view value)
    {
        return !value.empty() && value != "0" && value != "off" && value != "false";
    }

    class CsvWriter
    {
    public:
        CsvWriter(const char* environmentVariable, std::string header)
            : mEnvironmentVariable(environmentVariable)
            , mHeader(std::move(header))
        {
        }

        bool enabled()
        {
            ensureOpen();
            return mStream.is_open();
        }

        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mStream.is_open())
                return;

            std::lock_guard<std::mutex> lock(mMutex);
            mStream << line << '\n';
            if (++mLinesSinceFlush >= 60)
            {
                mStream.flush();
                mLinesSinceFlush = 0;
            }
        }

    private:
        void ensureOpen()
        {
            if (mAttempted.load(std::memory_order_acquire))
                return;

            std::lock_guard<std::mutex> lock(mMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                mPath = raw;
                mStream.open(std::filesystem::u8path(mPath), std::ios::out | std::ios::trunc);
                if (mStream.is_open())
                    mStream << mHeader << '\n';
            }

            mAttempted.store(true, std::memory_order_release);
        }

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        std::ofstream mStream;
        std::mutex mMutex;
        std::atomic<bool> mAttempted{ false };
        unsigned mLinesSinceFlush = 0;
    };

    inline std::string csvQuote(std::string_view value)
    {
        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');
        for (char c : value)
        {
            if (c == '"')
                result.push_back('"');
            result.push_back(c);
        }
        result.push_back('"');
        return result;
    }

    // Generic opt-in scope timer used by the Optimization Lab. The destination
    // writer decides which environment variable enables the stream. A minimum
    // duration keeps hot-path diagnostic files manageable.
    class ScopedCsvTimer
    {
    public:
        ScopedCsvTimer(CsvWriter& writer, std::string_view phase, std::string_view detail = {}, double minimumMs = 0.0)
            : mWriter(writer)
            , mPhase(phase)
            , mDetail(detail)
            , mMinimumMs(minimumMs)
            , mEnabled(writer.enabled())
            , mStart(mEnabled ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScopedCsvTimer()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << csvQuote(mPhase) << ','
                << csvQuote(mDetail) << ',' << std::fixed << std::setprecision(3) << durationMs;
            mWriter.writeLine(row.str());
        }

        ScopedCsvTimer(const ScopedCsvTimer&) = delete;
        ScopedCsvTimer& operator=(const ScopedCsvTimer&) = delete;

    private:
        CsvWriter& mWriter;
        std::string mPhase;
        std::string mDetail;
        double mMinimumMs;
        bool mEnabled;
        Clock::time_point mStart;
    };

    inline CsvWriter& transitionWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRANSITION_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& pagingWriter()
    {
        static CsvWriter writer("OPENMW_V3_PAGING_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& resourceWriter()
    {
        static CsvWriter writer("OPENMW_V3_RESOURCE_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& navWriter()
    {
        static CsvWriter writer("OPENMW_V3_NAV_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& insertionWriter()
    {
        static CsvWriter writer("OPENMW_V3_INSERT_FILE",
            "frame,epoch_ms,cell,total_refs,rendered_refs,physics_refs,actors,animated,doors,render_ms,mechanics_ms,"
            "particles_ms,physics_ms,lua_added_ms,nav_ms");
        return writer;
    }

    inline CsvWriter& workQueueWriter()
    {
        static CsvWriter writer("OPENMW_V3_WORKQUEUE_FILE",
            "frame,epoch_ms,thread,event,item,type,queue_depth,active_threads,duration_ms");
        return writer;
    }

    inline CsvWriter& renderWriter()
    {
        static CsvWriter writer("OPENMW_V3_RENDER_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& postFxWriter()
    {
        static CsvWriter writer("OPENMW_V3_POSTFX_FILE",
            "frame,epoch_ms,thread,technique,pass,cpu_submit_ms,width,height,render_target,mipmap");
        return writer;
    }

    inline CsvWriter& streamingWriter()
    {
        static CsvWriter writer("OPENMW_V3_STREAMING_FILE",
            "frame,epoch_ms,event,category,detail,last_frame_ms,limit,count");
        return writer;
    }

    inline CsvWriter& traceWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRACE_FILE",
            "frame,epoch_ms,thread,id,parent,category,name,detail,duration_ms");
        return writer;
    }

    // Nested, cross-thread trace scope. IDs and parent IDs let an offline tool
    // reconstruct the critical path instead of correlating unrelated CSV rows.
    class TraceScope
    {
    public:
        TraceScope(std::string_view category, std::string_view name, std::string_view detail = {}, double minimumMs = 0.0)
            : mCategory(category)
            , mName(name)
            , mDetail(detail)
            , mMinimumMs(minimumMs)
            , mEnabled(traceWriter().enabled())
        {
            if (!mEnabled)
                return;
            mId = sNextId.fetch_add(1, std::memory_order_relaxed);
            mParent = sCurrentParent;
            sCurrentParent = mId;
            mStart = Clock::now();
        }

        ~TraceScope()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            sCurrentParent = mParent;
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << threadId() << ',' << mId << ','
                << mParent << ',' << csvQuote(mCategory) << ',' << csvQuote(mName) << ',' << csvQuote(mDetail) << ','
                << std::fixed << std::setprecision(3) << durationMs;
            traceWriter().writeLine(row.str());
        }

        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;

    private:
        inline static std::atomic<unsigned long long> sNextId{ 1 };
        inline static thread_local unsigned long long sCurrentParent = 0;

        std::string mCategory;
        std::string mName;
        std::string mDetail;
        double mMinimumMs = 0.0;
        bool mEnabled = false;
        unsigned long long mId = 0;
        unsigned long long mParent = 0;
        Clock::time_point mStart{};
    };

    inline void writeEvent(std::string_view event, std::string_view detail = {})
    {
        static CsvWriter writer("OPENMW_V3_EVENT_FILE", "frame,epoch_ms,event,detail");
        if (!writer.enabled())
            return;

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << csvQuote(event) << ',' << csvQuote(detail);
        writer.writeLine(row.str());
    }
}

#endif
