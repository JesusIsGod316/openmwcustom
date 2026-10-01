#ifndef OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H
#define OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H

#include "v3hitchtelemetry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <concepts>
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
    enum class CpuTiming { Root, Scoped, None, WallEnvelope };

    struct Metric
    {
        std::array<char, 32> name{};
        std::uint64_t value = 0;
    };

    struct Row
    {
        std::uint64_t id, parent, thread;
        unsigned frame;
        std::int64_t start, end, epoch, children, cpu;
        bool truncated;
        std::array<char, 64> phase;
        std::array<char, 256> identity, asset;
        // A causal dependency can cross threads. It is never a nesting parent.
        std::uint64_t link = 0;
        std::array<Metric, 4> metrics{};
    };

    // One pre-touched arena shared by producers. No locks, allocation, text
    // formatting, file I/O, or GL calls in append(). Flush only after workers
    // and draw threads have joined. A full arena makes the capture incomplete.
    class Capture
    {
    public:
        static constexpr std::size_t Capacity = 262144;
        static constexpr std::size_t MaximumCapacity = 524288;
        static Capture& instance() { static Capture capture; return capture; }
        bool enabled() const { return mRows && !mStopped.load(std::memory_order_acquire); }
        std::uint64_t nextId() { return mIds.fetch_add(1, std::memory_order_relaxed) + 1; }
        void enter() { mActive.fetch_add(1, std::memory_order_acq_rel); }
        void leave() { mActive.fetch_sub(1, std::memory_order_acq_rel); }
        void append(const Row& row) noexcept
        {
            const auto index = mCount.fetch_add(1, std::memory_order_relaxed);
            if (index < mCapacity) mRows[index] = row;
            else mDropped.fetch_add(1, std::memory_order_relaxed);
        }
        void finish() noexcept
        {
            if (!requested() || mStopped.exchange(true)) return;
            try
            {
                writeClockMap(mPath, true);
                const auto active = mActive.load(std::memory_order_acquire);
                const auto count = (std::min)(mCount.load(), mCapacity);
                std::ofstream out(mPath);
                out << "span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link";
                for (unsigned i = 1; i <= 4; ++i) out << ",metric" << i << "_name,metric" << i << "_value";
                out << '\n';
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
                            << (r.cpu < 0 ? -1.0 : r.cpu / 1e6) << ',' << r.truncated << ',' << r.link;
                        for (const auto& metric : r.metrics)
                        { out << ','; quote(out, metric.name.data()); out << ',' << metric.value; }
                        out << '\n';
                    }
                out << "# rows=" << count << "\n# capacity=" << mCapacity << "\n# dropped=" << mDropped.load()
                    << "\n# allocation_failed=" << mAllocationFailed << "\n# active_at_finish=" << active
                    << "\n# minimum_detail_ms=0.25\n# schema=2\n# forced_dependency_events=retained\n# normal_finish=1\n";
                out.flush();
                const bool ok = bool(out) && !active && !mAllocationFailed;
                out.close();
                std::ofstream status(mPath + ".capture-status.txt");
                status << "rows=" << count << "\ncapacity=" << mCapacity << "\ndropped=" << mDropped.load()
                    << "\nallocation_failed=" << mAllocationFailed << "\nactive_at_finish=" << active
                    << "\nschema=2\nnormal_finish=1\noutput_ok=" << (ok && !out.fail()) << '\n';
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
                if (const char* capacity = std::getenv("OPENMW_P9_HITCH_CAPACITY"); capacity && *capacity)
                {
                    std::size_t parsed = 0;
                    const auto last = capacity + std::strlen(capacity);
                    const auto result = std::from_chars(capacity, last, parsed);
                    if (result.ec == std::errc{} && result.ptr == last
                        && (parsed == 131072 || parsed == Capacity || parsed == MaximumCapacity)) mCapacity = parsed;
                }
                writeClockMap(mPath, false);
                mRows = std::make_unique<Row[]>(mCapacity);
                volatile unsigned char* pages = reinterpret_cast<unsigned char*>(mRows.get());
                for (std::size_t i = 0; i < mCapacity * sizeof(Row); i += 4096) pages[i] = 0;
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
        std::size_t mCapacity = Capacity;
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
            mCpuTiming = cpu;
            mRow.id = mCapture->nextId();
            mRow.parent = mPrevious ? mPrevious->mRow.id : 0;
            mRow.frame = frame == std::numeric_limits<unsigned>::max()
                ? (mPrevious ? mPrevious->mRow.frame : V3HitchTelemetry::currentFrame()) : frame;
            mRow.thread = threadId();
            copy(mRow.phase, phase); copy(mRow.identity, identity); copy(mRow.asset, asset);
            // New wall-only attribution envelopes must not suppress a CPU
            // measurement that an existing Root scope took before the envelope
            // was introduced. Existing Root/Scoped/None nesting is unchanged.
            const Scope* cpuParent = mPrevious;
            while (cpuParent && cpuParent->mCpuTiming == CpuTiming::WallEnvelope)
                cpuParent = cpuParent->mPrevious;
            if (cpu == CpuTiming::Scoped || (cpu == CpuTiming::Root && !cpuParent))
                mCpuStart = threadCpuNs();
            mRow.epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            mRow.start = now();
            current = this;
        }
        // Constrain deduction so Scope(phase, {}, asset) selects the identity /
        // asset overload, rather than value-initializing an enum accidentally.
        template <typename Timing> requires std::same_as<Timing, CpuTiming>
        Scope(std::string_view phase, Timing cpu, std::string_view identity = {}, std::string_view asset = {}) noexcept
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
            const bool retain = mForced || mChildRetained || duration >= 250000;
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
        std::uint64_t id() const noexcept { return mCapture ? mRow.id : 0; }
        void retain() noexcept { mForced = true; }
        void setLink(std::uint64_t link) noexcept { mRow.link = link; }
        void setMetric(unsigned index, std::string_view name, std::uint64_t value) noexcept
        {
            if (!mCapture) return;
            if (index >= mRow.metrics.size()) { mRow.truncated = true; return; }
            auto& metric = mRow.metrics[index];
            metric = {};
            copy(metric.name, name);
            metric.value = value;
        }
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
        CpuTiming mCpuTiming = CpuTiming::None;
        bool mChildRetained = false;
        bool mForced = false;
    };
}
#endif
