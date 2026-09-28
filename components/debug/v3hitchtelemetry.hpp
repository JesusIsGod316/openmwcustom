#ifndef OPENMW_COMPONENTS_DEBUG_V3HITCHTELEMETRY_H
#define OPENMW_COMPONENTS_DEBUG_V3HITCHTELEMETRY_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <string>
#include <string_view>

#include "v33framestats.hpp"
#include "deferredcapture.hpp"

namespace Debug::V3HitchTelemetry
{
    constexpr std::size_t StageCount = 10;

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

    inline std::atomic<unsigned> sCurrentFrame{ 0 };
    inline std::atomic<double> sLastFrameWallMs{ 0.0 };

    inline unsigned currentFrame()
    {
        return sCurrentFrame.load(std::memory_order_relaxed);
    }

    inline double lastFrameWallMs()
    {
        return sLastFrameWallMs.load(std::memory_order_relaxed);
    }

    inline std::string telemetryPath()
    {
        if (const char* value = std::getenv("OPENMW_V3_HITCH_FILE"))
        {
            const std::string_view setting(value);
            if (setting.empty() || setting == "0" || setting == "off" || setting == "false")
                return {};
            return std::string(setting);
        }
        // Benchmark telemetry is opt-in. Normal gameplay must never start
        // writing hitch traces merely because a hitch occurred.
        return {};
    }

    class State
    {
    public:
        ~State() { finish(false); }

        void prepare()
        {
            if (!DeferredCapture::enabled() || mPrepared) return;
            mPrepared = true;
            mCleanFrames.prepare(131072);
        }

        void finish(bool includeLastFrame = true) noexcept
        {
            if (!DeferredCapture::enabled() || mFinished) return;
            mFinished = true;
            try
            {
                if (includeLastFrame && mStarted)
                    emitPreviousFrame(std::chrono::duration<double, std::milli>(Clock::now() - mFrameStart).count());
                mStarted = false;
                ensureAllFrameStream();
                ensureStream();
                for (const auto& record : mCleanFrames)
                {
                    auto write = [&](std::ostream& out) {
                        out << record.frame << ',' << record.epochMs << ',' << std::fixed << std::setprecision(3)
                            << record.wallMs;
                        for (double value : record.stages) out << ',' << value;
                        for (double value : record.tail) out << ',' << value;
                        out << ',' << record.accountedMs << ',' << record.otherMs;
                    };
                    if (mAllFrameStream.is_open()) { write(mAllFrameStream); mAllFrameStream << '\n'; }
                    if (mStream.is_open() && record.reason)
                    {
                        write(mStream);
                        mStream << ',' << (record.reason == 1 ? "hitch" : record.reason == 2 ? "slow_stage" : "baseline") << '\n';
                    }
                }
                mAllFrameStream.flush();
                mStream.flush();
                if (const char* file = std::getenv("OPENMW_V3_FRAME_FILE"); file && *file)
                {
                    std::ofstream status(std::filesystem::path(reinterpret_cast<const char8_t*>(file)) += ".capture-status.txt");
                    status << "format=p8g4-deferred-v1\nrows=" << mCleanFrames.size()
                           << "\ncapacity=" << mCleanFrames.capacity() << "\ndropped=" << mCleanFrames.dropped()
                           << "\nallocation_failed=" << mCleanFrames.allocationFailed()
                           << "\nnormal_finish=" << includeLastFrame
                           << "\nframe_output_ok=" << (mAllFrameStream.is_open() && mAllFrameStream.good())
                           << "\nlegacy_summary_suppressed=1\n";
                }
            }
            catch (...) { /* Shutdown cannot throw. Missing status makes the capture incomplete. */ }
        }

        void beginFrame(unsigned frameNumber)
        {
            prepare(); // The engine calls this before loading; stand-alone users remain safe.
            sCurrentFrame.store(frameNumber, std::memory_order_relaxed);
            const auto now = Clock::now();
            if (mStarted)
            {
                const double wallMs = std::chrono::duration<double, std::milli>(now - mFrameStart).count();
                sLastFrameWallMs.store(wallMs, std::memory_order_relaxed);
                emitPreviousFrame(wallMs);
            }

            mStarted = true;
            mFrame = frameNumber;
            mFrameStart = now;
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
        }

    private:
        using Clock = std::chrono::steady_clock;

        static constexpr double HitchThresholdMs = 25.0;
        static constexpr double StageThresholdMs = 10.0;
        static constexpr unsigned BaselineInterval = 120;
        static constexpr unsigned FlushInterval = 60;
        static constexpr unsigned AllFrameFlushInterval = 600;

        void ensureAllFrameStream()
        {
            if (mAllFrameStreamAttempted)
                return;
            mAllFrameStreamAttempted = true;

            const char* raw = std::getenv("OPENMW_V3_FRAME_FILE");
            if (!raw)
                return;
            const std::string_view setting(raw);
            if (setting.empty() || setting == "0" || setting == "off" || setting == "false")
                return;

            mAllFrameStream.open(std::filesystem::u8path(std::string(setting)), std::ios::out | std::ios::trunc);
            if (!mAllFrameStream.is_open())
                return;
            mAllFrameStream
                << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                   "world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,"
                   "rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms\n";
        }

        void emitAllFrame(double wallMs, double accountedMs, double otherMs)
        {
            ensureAllFrameStream();
            if (!mAllFrameStream.is_open())
                return;

            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();
            mAllFrameStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mAllFrameStream << ',' << value;
            for (double value : mFrameTailMs)
                mAllFrameStream << ',' << value;
            mAllFrameStream << ',' << accountedMs << ',' << otherMs << '\n';
            if (++mAllFrameLinesSinceFlush >= AllFrameFlushInterval)
            {
                mAllFrameStream.flush();
                mAllFrameLinesSinceFlush = 0;
            }
        }

        void ensureStream()
        {
            if (mStreamAttempted)
                return;
            mStreamAttempted = true;

            mPath = telemetryPath();
            if (mPath.empty())
                return;

            mStream.open(std::filesystem::u8path(mPath), std::ios::out | std::ios::trunc);
            if (!mStream.is_open())
                return;

            mStream << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                       "world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,"
                       "rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms,reason\n";
        }

        void emitPreviousFrame(double wallMs)
        {
            const double accountedMs
                = std::accumulate(mStageMs.begin(), mStageMs.end(), 0.0);
            const double tailAccountedMs
                = std::accumulate(mFrameTailMs.begin(), mFrameTailMs.end(), 0.0);
            const double totalAccountedMs = accountedMs + tailAccountedMs;
            const double otherMs = std::max(0.0, wallMs - totalAccountedMs);
            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());
            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();

            if (DeferredCapture::enabled())
            {
                const double largestTail = *std::max_element(mFrameTailMs.begin(), mFrameTailMs.end());
                const unsigned reason = wallMs >= HitchThresholdMs ? 1u
                    : std::max(largestStage, largestTail) >= StageThresholdMs ? 2u
                    : mFrame % BaselineInterval == 0 ? 3u : 0u;
                mCleanFrames.push({ mFrame, reason, epochMs, wallMs, mStageMs, mFrameTailMs, totalAccountedMs, otherMs });
                return;
            }
            V33FrameStats::record(mFrame, epochMs, wallMs);
            emitAllFrame(wallMs, totalAccountedMs, otherMs);

            const bool hitch = wallMs >= HitchThresholdMs;
            const double largestTailStage = *std::max_element(mFrameTailMs.begin(), mFrameTailMs.end());
            const bool slowStage = std::max(largestStage, largestTailStage) >= StageThresholdMs;
            const bool baseline = (mFrame % BaselineInterval) == 0;
            if (!hitch && !slowStage && !baseline)
                return;

            ensureStream();
            if (!mStream.is_open())
                return;

            const char* reason = hitch ? "hitch" : (slowStage ? "slow_stage" : "baseline");
            mStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mStream << ',' << value;
            for (double value : mFrameTailMs)
                mStream << ',' << value;
            mStream << ',' << totalAccountedMs << ',' << otherMs << ',' << reason << '\n';

            if (++mLinesSinceFlush >= FlushInterval)
            {
                mStream.flush();
                mLinesSinceFlush = 0;
            }
        }

        struct FrameRecord
        {
            unsigned frame, reason;
            std::int64_t epochMs;
            double wallMs;
            std::array<double, StageCount> stages;
            std::array<double, static_cast<std::size_t>(FrameTailStage::Count)> tail;
            double accountedMs, otherMs;
        };
        DeferredCapture::Buffer<FrameRecord> mCleanFrames;
        bool mPrepared = false, mFinished = false;
        bool mStarted = false;
        bool mStreamAttempted = false;
        bool mAllFrameStreamAttempted = false;
        unsigned mFrame = 0;
        unsigned mLinesSinceFlush = 0;
        unsigned mAllFrameLinesSinceFlush = 0;
        Clock::time_point mFrameStart{};
        std::chrono::system_clock::time_point mFrameStartSystem{};
        std::array<double, StageCount> mStageMs{};
        std::array<double, static_cast<std::size_t>(FrameTailStage::Count)> mFrameTailMs{};
        std::string mPath;
        std::ofstream mStream;
        std::ofstream mAllFrameStream;
    };

    inline State& state()
    {
        static State value;
        return value;
    }

    inline void beginFrame(unsigned frameNumber)
    {
        state().beginFrame(frameNumber);
    }

    inline void recordStage(std::size_t stage, double milliseconds)
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
}

#endif
