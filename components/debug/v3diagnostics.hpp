#ifndef OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H
#define OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

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
