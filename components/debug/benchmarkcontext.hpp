#ifndef OPENMW_DEBUG_BENCHMARKCONTEXT_H
#define OPENMW_DEBUG_BENCHMARKCONTEXT_H

#include "deferredcapture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <utility>

namespace Debug::BenchmarkContext
{
    // Observed at main-frame entry, after state/input updates and at exit. These
    // are state receipts, not a physics-time proxy or a presentation interval.
    enum Flag : unsigned
    {
        Paused = 1,
        Gui = 2,
        MainMenu = 4,
        Hidden = 8,
        NotRunning = 16,
        LoadingAdvancedViewer = 32,
        FrameException = 64,
    };

    struct Frame
    {
        unsigned frame, viewerEnd, beginFlags, endFlags, observedFlags;
    };

    struct Snapshot
    {
        unsigned frame = 0;
        bool valid = false, exterior = false, cellTruncated = false;
        int cameraMode = -1, weather = -1, day = -1;
        double hour = 0, weatherTransition = 0;
        std::array<double, 3> player{}, camera{}, rotation{};
        std::array<char, 192> cell{};

        void setCell(std::string_view name) noexcept
        {
            const auto count = std::min(name.size(), cell.size() - 1);
            std::copy_n(name.data(), count, cell.data());
            cell[count] = '\0';
            cellTruncated = count != name.size();
        }
    };

    class Capture
    {
    public:
        void prepare(std::string path = {})
        {
            if (mPrepared) return;
            mPrepared = true;
            if (path.empty())
            {
                const char* value = std::getenv("OPENMW_P9_BENCHMARK_CONTEXT_FILE");
                if (!value || !*value || std::string_view(value) == "0") return;
                path = value;
            }
            mPath = std::move(path);
            mFrames.prepare(131072);
            mSnapshots.prepare(8192);
        }

        bool enabled() const noexcept { return !mPath.empty() && !mFinished; }

        void begin(unsigned frame, unsigned flags) noexcept
        {
            if (!enabled()) return;
            if (mPending) ++mUnfinished;
            mCurrent = { frame, frame, flags, flags, flags };
            mPending = true;
            mSnapshotDue = !mHavePrevious || flags != mPreviousFlags || frame >= mNextSnapshot;
        }

        void observe(unsigned flags) noexcept
        {
            if (!enabled() || !mPending) return;
            if (flags != mCurrent.endFlags) mSnapshotDue = true;
            mCurrent.endFlags = flags;
            mCurrent.observedFlags |= flags;
        }

        bool snapshotDue() const noexcept { return enabled() && mPending && mSnapshotDue; }

        void snapshot(const Snapshot& value) noexcept
        {
            if (!snapshotDue() || value.frame != mCurrent.frame) return;
            mSnapshots.push(value);
            mNextSnapshot = value.frame + 60;
            mSnapshotDue = false;
        }

        void end(unsigned viewerFrame, unsigned flags) noexcept
        {
            if (!enabled() || !mPending) return;
            observe(flags);
            mCurrent.viewerEnd = viewerFrame;
            if (viewerFrame != mCurrent.frame)
                mCurrent.observedFlags |= LoadingAdvancedViewer;
            mFrames.push(mCurrent);
            mPreviousFlags = flags;
            mHavePrevious = true;
            mPending = false;
        }

        void finish(bool normal) noexcept
        {
            if (!enabled()) return;
            mFinished = true;
            if (mPending) ++mUnfinished;
            try
            {
                const auto path = std::filesystem::u8path(mPath);
                std::ofstream frames(path);
                frames << "frame,viewer_end_frame,begin_flags,end_flags,observed_flags\n";
                for (const auto& row : mFrames)
                    frames << row.frame << ',' << row.viewerEnd << ',' << row.beginFlags << ',' << row.endFlags
                           << ',' << row.observedFlags << '\n';
                frames.flush();
                std::ofstream snapshots(std::filesystem::path(path) += ".snapshots.csv");
                snapshots << "frame,valid,exterior,camera_mode,weather,day,hour,weather_transition,player_x,player_y,"
                             "player_z,camera_x,camera_y,camera_z,yaw,pitch,roll,cell,cell_truncated\n";
                snapshots << std::setprecision(17);
                for (const auto& row : mSnapshots)
                {
                    snapshots << row.frame << ',' << row.valid << ',' << row.exterior << ',' << row.cameraMode
                              << ',' << row.weather << ',' << row.day << ',' << row.hour << ',' << row.weatherTransition;
                    for (double v : row.player) snapshots << ',' << v;
                    for (double v : row.camera) snapshots << ',' << v;
                    for (double v : row.rotation) snapshots << ',' << v;
                    snapshots << ",\"";
                    for (char c : row.cell)
                    {
                        if (!c) break;
                        if (c == '"') snapshots << '"';
                        snapshots << (c == '\n' || c == '\r' ? ' ' : c);
                    }
                    snapshots << "\"," << row.cellTruncated << '\n';
                }
                snapshots.flush();
                const bool written = frames.good() && snapshots.good();
                frames.close();
                snapshots.close();
                std::ofstream status(std::filesystem::path(path) += ".capture-status.txt");
                status << "schema=1\nnormal_finish=" << normal << "\nrows=" << mFrames.size()
                       << "\nsnapshot_rows=" << mSnapshots.size() << "\ndropped=" << mFrames.dropped()
                       << "\nsnapshots_dropped=" << mSnapshots.dropped() << "\nunfinished_frames=" << mUnfinished
                       << "\nallocation_failed=" << (mFrames.allocationFailed() || mSnapshots.allocationFailed())
                       << "\noutput_ok=" << (written && !frames.fail() && !snapshots.fail())
                       << "\nstate_observation=main_entry_after_state_and_main_exit\n"
                          "snapshot_observation=main_exit_before_frame_limiter;maximum_60_engine_frames\n"
                          "route_phase=not_attested;pose_samples_do_not_prove_user_route_identity\n";
            }
            catch (...) { /* Missing output/status makes this channel incomplete. */ }
        }

        ~Capture() { finish(false); }

    private:
        std::string mPath;
        DeferredCapture::Buffer<Frame> mFrames;
        DeferredCapture::Buffer<Snapshot> mSnapshots;
        Frame mCurrent{};
        unsigned mPreviousFlags = 0, mNextSnapshot = 0;
        std::uint64_t mUnfinished = 0;
        bool mPrepared = false, mFinished = false, mPending = false, mHavePrevious = false, mSnapshotDue = false;
    };

    inline Capture& capture()
    {
        static Capture result;
        return result;
    }
}

#endif
