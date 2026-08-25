#ifndef OPENMW_COMPONENTS_DEBUG_V3HITCHTELEMETRY_H
#define OPENMW_COMPONENTS_DEBUG_V3HITCHTELEMETRY_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <string>
#include <string_view>

namespace Debug::V3HitchTelemetry
{
    constexpr std::size_t StageCount = 10;

    inline std::string telemetryPath()
    {
        if (const char* value = std::getenv("OPENMW_V3_HITCH_FILE"))
        {
            const std::string_view setting(value);
            if (setting.empty() || setting == "0" || setting == "off" || setting == "false")
                return {};
            return std::string(setting);
        }
        return "v3-hitch-telemetry.csv";
    }

    class State
    {
    public:
        void beginFrame(unsigned frameNumber)
        {
            const auto now = Clock::now();
            if (mStarted)
            {
                const double wallMs = std::chrono::duration<double, std::milli>(now - mFrameStart).count();
                emitPreviousFrame(wallMs);
            }

            mStarted = true;
            mFrame = frameNumber;
            mFrameStart = now;
            mFrameStartSystem = std::chrono::system_clock::now();
            mStageMs.fill(0.0);
        }

        void recordStage(std::size_t stage, double milliseconds)
        {
            if (!mStarted || stage >= mStageMs.size())
                return;
            mStageMs[stage] += milliseconds;
        }

    private:
        using Clock = std::chrono::steady_clock;

        static constexpr double HitchThresholdMs = 25.0;
        static constexpr double StageThresholdMs = 10.0;
        static constexpr unsigned BaselineInterval = 120;
        static constexpr unsigned FlushInterval = 60;

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
                       "world_ms,gui_ms,focus_ms,accounted_ms,other_ms,reason\n";
        }

        void emitPreviousFrame(double wallMs)
        {
            const double accountedMs
                = std::accumulate(mStageMs.begin(), mStageMs.end(), 0.0);
            const double otherMs = std::max(0.0, wallMs - accountedMs);
            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());

            const bool hitch = wallMs >= HitchThresholdMs;
            const bool slowStage = largestStage >= StageThresholdMs;
            const bool baseline = (mFrame % BaselineInterval) == 0;
            if (!hitch && !slowStage && !baseline)
                return;

            ensureStream();
            if (!mStream.is_open())
                return;

            const char* reason = hitch ? "hitch" : (slowStage ? "slow_stage" : "baseline");
            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();
            mStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mStream << ',' << value;
            mStream << ',' << accountedMs << ',' << otherMs << ',' << reason << '\n';

            if (++mLinesSinceFlush >= FlushInterval)
            {
                mStream.flush();
                mLinesSinceFlush = 0;
            }
        }

        bool mStarted = false;
        bool mStreamAttempted = false;
        unsigned mFrame = 0;
        unsigned mLinesSinceFlush = 0;
        Clock::time_point mFrameStart{};
        std::chrono::system_clock::time_point mFrameStartSystem{};
        std::array<double, StageCount> mStageMs{};
        std::string mPath;
        std::ofstream mStream;
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
}

#endif
