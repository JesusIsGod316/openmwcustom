#ifndef OPENMW_COMPONENTS_DEBUG_V33FRAMESTATS_H
#define OPENMW_COMPONENTS_DEBUG_V33FRAMESTATS_H

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace Debug::V33FrameStats
{
    class State
    {
    public:
        ~State() { emitWindow(); }

        void record(unsigned frame, long long epochMs, double wallMs)
        {
            ensureStream();
            if (!mStream.is_open())
                return;

            mLastFrame = frame;
            mLastEpochMs = epochMs;
            mSamples.push_back(wallMs);
            mCount25 += wallMs > 25.0;
            mCount33 += wallMs > (1000.0 / 30.0);
            mCount50 += wallMs > 50.0;
            mCount100 += wallMs > 100.0;
            if (mSamples.size() >= WindowSize)
                emitWindow();
        }

    private:
        static constexpr std::size_t WindowSize = 300;

        void ensureStream()
        {
            if (mAttempted)
                return;
            mAttempted = true;

            const char* raw = std::getenv("OPENMW_V33_FRAME_SUMMARY_FILE");
            if (!raw)
                return;
            const std::string_view value(raw);
            if (value.empty() || value == "0" || value == "off" || value == "false")
                return;

            mStream.open(std::filesystem::u8path(std::string(value)), std::ios::out | std::ios::trunc);
            if (mStream.is_open())
                mStream << "end_frame,epoch_ms,samples,mean_ms,median_ms,p95_ms,p99_ms,p99_5_ms,worst_ms,gt25_ms,"
                           "gt33_3_ms,gt50_ms,gt100_ms\n";
            mSamples.reserve(WindowSize);
        }

        static double percentile(const std::vector<double>& values, double fraction)
        {
            if (values.empty())
                return 0.0;
            const std::size_t index = std::min(values.size() - 1,
                static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
            return values[index];
        }

        void emitWindow()
        {
            if (!mStream.is_open() || mSamples.empty())
                return;

            std::sort(mSamples.begin(), mSamples.end());
            const double mean
                = std::accumulate(mSamples.begin(), mSamples.end(), 0.0) / static_cast<double>(mSamples.size());
            mStream << mLastFrame << ',' << mLastEpochMs << ',' << mSamples.size() << ',' << std::fixed
                    << std::setprecision(3) << mean << ',' << percentile(mSamples, 0.5) << ','
                    << percentile(mSamples, 0.95) << ',' << percentile(mSamples, 0.99) << ','
                    << percentile(mSamples, 0.995) << ',' << mSamples.back() << ',' << mCount25 << ',' << mCount33
                    << ',' << mCount50 << ',' << mCount100 << '\n';
            mStream.flush();
            mSamples.clear();
            mCount25 = 0;
            mCount33 = 0;
            mCount50 = 0;
            mCount100 = 0;
        }

        bool mAttempted = false;
        unsigned mLastFrame = 0;
        long long mLastEpochMs = 0;
        unsigned mCount25 = 0;
        unsigned mCount33 = 0;
        unsigned mCount50 = 0;
        unsigned mCount100 = 0;
        std::vector<double> mSamples;
        std::ofstream mStream;
    };

    inline void record(unsigned frame, long long epochMs, double wallMs)
    {
        static State state;
        state.record(frame, epochMs, wallMs);
    }
}

#endif
