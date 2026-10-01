#ifndef OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H
#define OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H

#include "v3hitchtelemetry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
namespace Debug::HitchTrace
{
    using Clock = std::chrono::steady_clock;
    inline bool requested()
    {
        static const bool value = [] {
            const char* path = std::getenv("OPENMW_P9_HITCH_SPANS_FILE");
            return path && *path && std::string_view(path) != "0";
        }();
        return value;
    }

    // OS thread CPU time is coarse on Windows; it is not a driver/GPU timer.
    std::int64_t threadCpuNs() noexcept;
    std::uint64_t threadId() noexcept;
    void writeClockMap(const std::string& path, bool final) noexcept;
    enum class CpuTiming { Root, Scoped };

    struct Row
    {
        std::uint64_t id, parent, thread;
        unsigned frame;
        std::int64_t start, end, epoch, children, cpu;
        bool truncated;
        std::array<char, 64> phase;
        std::array<char, 256> identity, asset;
    };

    // One pre-touched arena shared by producers. No locks, allocation, text
    // formatting, file I/O, or GL calls in append(). Flush only after workers
    // and draw threads have joined. A full arena makes the capture incomplete.
    class Capture
    {
    public:
        static constexpr std::size_t Capacity = 262144;
        static Capture& instance() { static Capture capture; return capture; }
        bool enabled() const { return mRows && !mStopped.load(std::memory_order_acquire); }
        std::uint64_t nextId() { return mIds.fetch_add(1, std::memory_order_relaxed) + 1; }
        void enter() { mActive.fetch_add(1, std::memory_order_acq_rel); }
        void leave() { mActive.fetch_sub(1, std::memory_order_acq_rel); }
        void append(const Row& row) noexcept
        {
            const auto index = mCount.fetch_add(1, std::memory_order_relaxed);
            if (index < Capacity) mRows[index] = row;
            else mDropped.fetch_add(1, std::memory_order_relaxed);
        }
        void finish() noexcept
        {
            if (!requested() || mStopped.exchange(true)) return;
            try
            {
                writeClockMap(mPath, true);
                const auto active = mActive.load(std::memory_order_acquire);
                const auto count = (std::min)(mCount.load(), Capacity);
                std::ofstream out(mPath);
                out << "span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated\n";
                if (!active && mRows)
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const Row& r = mRows[i];
                        const auto duration = r.end - r.start;
                        out << r.id << ',' << r.parent << ',' << r.frame << ',' << r.thread << ',' << r.start
                            << ',' << r.end << ',' << r.epoch << ',';
                        quote(out, r.phase.data()); out << ','; quote(out, r.identity.data()); out << ',';
                        quote(out, r.asset.data());
                        out << std::fixed << std::setprecision(6) << ',' << duration / 1e6 << ',' << r.children / 1e6
                            << ',' << (std::max)(std::int64_t(0), duration - r.children) / 1e6 << ','
                            << (r.cpu < 0 ? -1.0 : r.cpu / 1e6) << ',' << r.truncated << '\n';
                    }
                out << "# rows=" << count << "\n# capacity=" << Capacity << "\n# dropped=" << mDropped.load()
                    << "\n# allocation_failed=" << mAllocationFailed << "\n# active_at_finish=" << active
                    << "\n# minimum_detail_ms=0.25\n# normal_finish=1\n";
                out.flush();
                const bool ok = bool(out) && !active && !mAllocationFailed;
                out.close();
                std::ofstream status(mPath + ".capture-status.txt");
                status << "rows=" << count << "\ncapacity=" << Capacity << "\ndropped=" << mDropped.load()
                    << "\nallocation_failed=" << mAllocationFailed << "\nactive_at_finish=" << active
                    << "\nnormal_finish=1\noutput_ok=" << (ok && !out.fail()) << '\n';
            }
            catch (...) { /* Missing/incomplete sidecar fails archive qualification. */ }
        }
        ~Capture() { finish(); }
    private:
        Capture() noexcept
        {
            if (!requested()) return;
            try
            {
                mPath = std::getenv("OPENMW_P9_HITCH_SPANS_FILE");
                writeClockMap(mPath, false);
                mRows = std::make_unique<Row[]>(Capacity);
                volatile unsigned char* pages = reinterpret_cast<unsigned char*>(mRows.get());
                for (std::size_t i = 0; i < Capacity * sizeof(Row); i += 4096) pages[i] = 0;
            }
            catch (...) { mAllocationFailed = true; }
        }
        static void quote(std::ostream& out, std::string_view text)
        {
            out << '"';
            for (char c : text) { if (c == '"') out << '"'; out << c; }
            out << '"';
        }
        std::string mPath;
        std::unique_ptr<Row[]> mRows;
        std::atomic<std::uint64_t> mIds{0};
        std::atomic<std::size_t> mCount{0}, mDropped{0}, mActive{0};
        std::atomic<bool> mStopped{false};
        bool mAllocationFailed = false;
    };

    class Scope;
    inline thread_local Scope* current = nullptr;
    class Scope
    {
    public:
        explicit Scope(std::string_view phase, std::string_view identity = {}, std::string_view asset = {},
            unsigned frame = std::numeric_limits<unsigned>::max(), CpuTiming cpu = CpuTiming::Root) noexcept
        {
            if (!requested() || !Capture::instance().enabled()) return;
            mRow = {};
            mRow.cpu = -1;
            mCapture = &Capture::instance();
            mCapture->enter();
            mPrevious = current;
            mRow.id = mCapture->nextId();
            mRow.parent = mPrevious ? mPrevious->mRow.id : 0;
            mRow.frame = frame == std::numeric_limits<unsigned>::max()
                ? (mPrevious ? mPrevious->mRow.frame : V3HitchTelemetry::currentFrame()) : frame;
            mRow.thread = threadId();
            copy(mRow.phase, phase); copy(mRow.identity, identity); copy(mRow.asset, asset);
            if (!mPrevious || cpu == CpuTiming::Scoped) mCpuStart = threadCpuNs();
            mRow.epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            mRow.start = now();
            current = this;
        }
        Scope(std::string_view phase, CpuTiming cpu, std::string_view identity = {}, std::string_view asset = {}) noexcept
            : Scope(phase, identity, asset, std::numeric_limits<unsigned>::max(), cpu) {}
        ~Scope()
        {
            if (!mCapture) return;
            mRow.end = now();
            const auto duration = mRow.end - mRow.start;
            if (mCpuStart >= 0)
            {
                const auto end = threadCpuNs();
                if (end >= mCpuStart) mRow.cpu = end - mCpuStart;
            }
            const bool retain = mChildRetained || duration >= 250000;
            if (mPrevious)
            {
                mPrevious->mRow.children += duration;
                mPrevious->mChildRetained |= retain;
            }
            current = mPrevious;
            if (retain) mCapture->append(mRow);
            mCapture->leave();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        static std::int64_t now()
        { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
        template<std::size_t N> void copy(std::array<char, N>& out, std::string_view text) noexcept
        {
            const auto n = (std::min)(text.size(), N - 1);
            if (n) std::memcpy(out.data(), text.data(), n);
            mRow.truncated |= text.size() >= N;
        }
        Row mRow;
        Capture* mCapture = nullptr;
        Scope* mPrevious = nullptr;
        std::int64_t mCpuStart = -1;
        bool mChildRetained = false;
    };
}
#endif
