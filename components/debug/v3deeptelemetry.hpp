#ifndef OPENMW_COMPONENTS_DEBUG_V3DEEPTELEMETRY_H
#define OPENMW_COMPONENTS_DEBUG_V3DEEPTELEMETRY_H

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

namespace Debug::V324DeepTelemetry
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

    inline std::string csvQuote(std::string_view value)
    {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (char c : value)
        {
            if (c == '"')
                out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    inline bool enabled()
    {
        static const bool value = [] {
            const char* raw = std::getenv("OPENMW_V324_DEEP_TELEMETRY");
            return raw && raw[0] == '1' && raw[1] == '\0';
        }();
        return value;
    }

    struct WriterCost
    {
        double formatMs = 0.0;
        double lockWaitMs = 0.0;
        double openMs = 0.0;
        double writeMs = 0.0;
        double flushMs = 0.0;
        std::size_t bytes = 0;
    };

    class Writer
    {
    public:
        void write(std::string_view category, std::string_view name, std::string_view detail, double bodyMs,
            double scopeSetupMs)
        {
            if (!enabled())
                return;

            const auto lockStart = Clock::now();
            std::unique_lock lock(mMutex);
            const double lockWaitMs = elapsedMs(lockStart);

            double openMs = 0.0;
            if (!mAttempted)
            {
                const auto openStart = Clock::now();
                const char* raw = std::getenv("OPENMW_V324_DEEP_FILE");
                if (raw && raw[0] != '\0')
                {
                    mStream.open(std::filesystem::u8path(raw), std::ios::out | std::ios::trunc);
                    if (mStream.is_open())
                    {
                        mStream << "frame,epoch_ms,thread,category,name,detail,body_ms,scope_setup_ms,"
                                   "prev_format_ms,prev_lock_wait_ms,prev_open_ms,prev_write_ms,prev_flush_ms,prev_bytes\n";
                    }
                }
                mAttempted = true;
                openMs = elapsedMs(openStart);
            }

            if (!mStream.is_open())
                return;

            const WriterCost previous = mPrevious;
            const auto formatStart = Clock::now();
            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << threadId() << ','
                << csvQuote(category) << ',' << csvQuote(name) << ',' << csvQuote(detail) << ',' << std::fixed
                << std::setprecision(6) << bodyMs << ',' << scopeSetupMs << ',' << previous.formatMs << ','
                << previous.lockWaitMs << ',' << previous.openMs << ',' << previous.writeMs << ',' << previous.flushMs
                << ',' << previous.bytes;
            const std::string line = row.str();
            const double formatMs = elapsedMs(formatStart);

            const auto writeStart = Clock::now();
            mStream << line << '\n';
            const double writeMs = elapsedMs(writeStart);

            double flushMs = 0.0;
            if (++mRowsSinceFlush >= 64)
            {
                const auto flushStart = Clock::now();
                mStream.flush();
                flushMs = elapsedMs(flushStart);
                mRowsSinceFlush = 0;
            }

            mPrevious = WriterCost{ formatMs, lockWaitMs, openMs, writeMs, flushMs, line.size() + 1 };
        }

    private:
        std::mutex mMutex;
        std::ofstream mStream;
        bool mAttempted = false;
        unsigned mRowsSinceFlush = 0;
        WriterCost mPrevious;
    };

    inline Writer& writer()
    {
        static Writer instance;
        return instance;
    }

    class Scope
    {
    public:
        Scope(std::string_view category, std::string_view name, std::string_view detail = {})
            : mEnabled(enabled())
        {
            // Keep telemetry-OFF useful as an identical-binary observer-effect
            // control: do not touch the high-resolution clock or allocate/copy
            // strings unless the deep profiler is actually enabled.
            if (!mEnabled)
                return;

            const auto setupStart = Clock::now();
            mCategory.assign(category);
            mName.assign(name);
            mDetail.assign(detail);
            mStart = Clock::now();
            mSetupMs = elapsedMs(setupStart);
        }

        ~Scope()
        {
            if (!mEnabled)
                return;
            const auto end = Clock::now();
            writer().write(mCategory, mName, mDetail, elapsedMs(mStart, end), mSetupMs);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        bool mEnabled = false;
        std::string mCategory;
        std::string mName;
        std::string mDetail;
        Clock::time_point mStart{};
        double mSetupMs = 0.0;
    };

    inline void event(std::string_view category, std::string_view name, std::string_view detail = {})
    {
        if (!enabled())
            return;
        writer().write(category, name, detail, 0.0, 0.0);
    }
}

#endif
