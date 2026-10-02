#ifndef OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H
#define OPENMW_COMPONENTS_DEBUG_HITCHTRACE_H

#include "v3hitchtelemetry.hpp"
#include "diagnosticobserver.hpp"

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
#include <condition_variable>
#include <mutex>
#include <new>
#include <type_traits>
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
    static_assert(std::is_trivially_destructible_v<Row>);

    // Producers copy owned fixed-size records into pre-touched storage. Only
    // the consumer formats/writes streaming records. Neither transport waits,
    // allocates, performs file I/O or calls GL on a producer. The deferred arena
    // remains an independently selectable recorder control.
    class Capture
    {
    public:
        static constexpr std::size_t Capacity = 262144;
        static constexpr std::size_t MaximumCapacity = 524288;
        static Capture& instance() { static Capture capture; return capture; }
        struct Options
        {
            std::string path;
            bool streaming = false;
            std::size_t capacity = Capacity;
            // Native test seam: no environment variable enables this hook in
            // the engine. Its context must outlive finish()/writer join.
            void (*beforeConsumerRead)(void*) noexcept = nullptr;
            void* consumerContext = nullptr;
        };
        explicit Capture(Options options) noexcept;
        bool enabled() const { return mArmed && !mStopped.load(std::memory_order_acquire); }
        std::uint64_t nextId() { return mIds.fetch_add(1, std::memory_order_relaxed) + 1; }
        void enter() { mActive.fetch_add(1, std::memory_order_acq_rel); }
        void leave() { mActive.fetch_sub(1, std::memory_order_acq_rel); }
        void append(const Row& row) noexcept;
        std::uint64_t pendingRows() const noexcept { return mPending.load(std::memory_order_acquire); }
        void finish() noexcept;
        ~Capture();
    private:
        Capture() noexcept;
        void initialize(Options options) noexcept;
        struct Slot { std::atomic<std::size_t> sequence{0}; Row row{}; };
        enum class PushResult { Published, Full, Contended };
        PushResult push(const Row& row) noexcept;
        bool pop(Row& row) noexcept;
        void writerEntry() noexcept;
        void writeHeader(std::ostream& out);
        void writeRow(std::ostream& out, const Row& row);
        void writeStatus(std::ostream& out, std::string_view prefix, bool outputOk);
        static void quote(std::ostream& out, std::string_view text);
        static void nameWriter() noexcept;
        static std::int64_t now() noexcept;
        std::string mPath;
        std::size_t mCapacity = Capacity;
        bool mArmed = false, mStreaming = false;
        std::unique_ptr<Row[]> mRows;
        std::unique_ptr<Slot[]> mSlots;
        std::atomic<std::size_t> mProducer{0}, mConsumer{0}, mQueued{0}, mHighWater{0};
        std::atomic<std::uint64_t> mAttempted{0}, mAccepted{0}, mWritten{0}, mPending{0};
        std::atomic<std::uint64_t> mCapacityDropped{0}, mContentionDropped{0}, mAllocationDropped{0},
            mIoDropped{0}, mStoppedDropped{0};
        std::atomic<std::uint64_t> mIds{0};
        std::atomic<std::size_t> mActive{0}, mAppenders{0};
        std::atomic<bool> mStopped{false}, mFinishing{false};
        bool mAllocationFailed = false, mInitializationFailed = false, mWriterFailed = false;
        bool mDrainComplete = false, mOutputOk = false;
        std::size_t mActiveAtFinish = 0, mAppendersAtFinish = 0;
        std::uint64_t mWriterThreadId = 0, mFormatNs = 0, mWriteNs = 0, mBatches = 0, mBytes = 0;
        std::int64_t mWriterStarted = 0, mWriterFinished = 0, mWriterCpu = -1;
        std::thread mWriter;
        std::mutex mWakeMutex;
        std::condition_variable mWake;
        void (*mBeforeConsumerRead)(void*) noexcept = nullptr;
        void* mConsumerContext = nullptr;
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
            const auto observerStart = DiagnosticObserver::begin();
            // Row's Metric defaults otherwise initialize trace payload before
            // the disabled gate. Construct owned storage only for a real span.
            ::new (static_cast<void*>(&mRow)) Row{};
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
            DiagnosticObserver::record(DiagnosticObserver::Category::TraceScopeEntry, observerStart, mRow.start);
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
            DiagnosticObserver::count(retain ? DiagnosticObserver::Category::TraceRetained
                                            : DiagnosticObserver::Category::TraceFiltered);
            if (mPrevious)
            {
                mPrevious->mRow.children += duration;
                mPrevious->mChildRetained |= retain;
            }
            current = mPrevious;
            if (retain) mCapture->append(mRow);
            mCapture->leave();
            DiagnosticObserver::end(DiagnosticObserver::Category::TraceScopeExit, mRow.end);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        std::uint64_t id() const noexcept { return mCapture ? mRow.id : 0; }
        std::int64_t startNs() const noexcept { return mCapture ? mRow.start : 0; }
        unsigned frame() const noexcept { return mCapture ? mRow.frame : V3HitchTelemetry::currentFrame(); }
        void retain() noexcept { mForced = true; }
        void setLink(std::uint64_t link) noexcept { if (mCapture) mRow.link = link; }
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
        // Row is trivially destructible; inactive scopes never begin its
        // lifetime and must not touch it. Active records remain fully owned.
        union { Row mRow; };
        Capture* mCapture = nullptr;
        Scope* mPrevious = nullptr;
        std::int64_t mCpuStart = -1;
        CpuTiming mCpuTiming = CpuTiming::None;
        bool mChildRetained = false;
        bool mForced = false;
    };
}
#endif
