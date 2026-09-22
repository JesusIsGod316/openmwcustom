#ifndef OPENMW_DEBUG_RUNTIMEDIAGNOSTICS_H
#define OPENMW_DEBUG_RUNTIMEDIAGNOSTICS_H

// Optional observer, independent of cache policy and renderer selection. Hot
// producers copy fixed records with try_lock: no I/O, heap allocation, blocking
// queue, GPU fence, or live scene traversal. The writer sees copies only.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Debug::RuntimeDiagnostics
{
    enum class Mode { Off, Standard, Focused };
    inline Mode mode() noexcept
    {
        static const Mode value = [] {
            const char* env = std::getenv("OPENMW_RUNTIME_DIAGNOSTICS");
            if (!env) return Mode::Off;
            const std::string_view text(env);
            if (text == "standard" || text == "1") return Mode::Standard;
            if (text == "focused") return Mode::Focused;
            return Mode::Off;
        }();
        return value;
    }
    inline bool enabled() noexcept { return mode() != Mode::Off; }
    using Clock = std::chrono::steady_clock;
    inline std::uint64_t nowUs() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    }
    inline std::atomic<std::uint64_t> currentFrame{ 0 };
    using ProcessProbe = void (*)() noexcept;
    inline std::atomic<ProcessProbe> processProbe{ nullptr };
    inline std::atomic<std::uint64_t> producerUs{ 0 }, attempted{ 0 }, dropped{ 0 }, clipped{ 0 };

    template <std::size_t N>
    inline bool copyText(std::array<char, N>& out, std::string_view in) noexcept
    {
        const auto size = (std::min)(N - 1, in.size());
        if (size) std::memcpy(out.data(), in.data(), size);
        bool embeddedNull = false;
        for (std::size_t i = 0; i < size; ++i)
            if (!out[i]) { out[i] = '?'; embeddedNull = true; }
        out[size] = 0;
        return size != in.size() || embeddedNull;
    }
    struct Value
    {
        std::array<char, 48> key{};
        std::array<char, 256> text{};
        std::uint64_t number = 0;
    };
    struct Record
    {
        std::uint64_t timeUs = 0, frame = 0;
        std::array<char, 48> type{};
        std::array<char, 96> owner{};
        std::array<char, 256> identity{};
        std::array<Value, 16> values{};
        unsigned count = 0;
        bool legacy = false, truncated = false;
    };
    inline constexpr std::size_t QueueCapacity = 1024;
    inline constexpr std::size_t QueueShards = 8;
    inline constexpr std::uint64_t FileLimit = 64ull * 1024 * 1024;

    // A bounded ring whose synchronization is supplied by Recorder. Exposed for
    // behavioral tests; overflow drops the newest event, never corrupts history.
    template <std::size_t N>
    class Ring
    {
    public:
        bool push(const Record& record) noexcept
        {
            if (mSize == N) return false;
            mRecords[(mRead + mSize) % N] = record;
            ++mSize;
            return true;
        }
        bool pop(Record& record) noexcept
        {
            if (!mSize) return false;
            record = mRecords[mRead];
            mRead = (mRead + 1) % N;
            --mSize;
            return true;
        }
        std::size_t size() const noexcept { return mSize; }
    private:
        std::array<Record, N> mRecords{};
        std::size_t mRead = 0, mSize = 0;
    };

    inline void quote(std::ostream& out, const char* text)
    {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
        {
            if (*p == '"' || *p == '\\') out << '\\' << char(*p);
            else if (*p < 32) out << "\\u00" << hex[*p >> 4] << hex[*p & 15];
            else out << char(*p);
        }
        out << '"';
    }
    inline void writeRecord(std::ostream& out, const Record& record)
    {
        out << "{\"schema\":" << (record.legacy ? 1 : 2) << ",\"frame\":" << record.frame
            << ",\"time_us\":" << record.timeUs << ",\"type\":";
        quote(out, record.type.data());
        if (!record.legacy)
        {
            out << ",\"owner\":"; quote(out, record.owner.data());
            out << ",\"identity\":"; quote(out, record.identity.data());
        }
        if (record.truncated) out << ",\"truncated\":true";
        for (unsigned i = 0; i < record.count; ++i)
        {
            out << ','; quote(out, record.values[i].key.data()); out << ':';
            if (record.legacy) quote(out, record.values[i].text.data());
            else out << record.values[i].number;
        }
        out << "}\n";
    }

    inline bool writeBoundedRecord(std::ostream& stream, const Record& record, bool& capped,
        std::uint64_t limit = FileLimit)
    {
        if (!stream || capped) return false;
        const auto position = stream.tellp();
        if (position < 0) return false;
        if (static_cast<std::uint64_t>(position) >= limit)
        {
            stream << "{\"schema\":" << (record.legacy ? 1 : 2)
                << ",\"type\":\"capture_limit\",\"frame\":" << record.frame << "}\n";
            capped = true;
            return false;
        }
        writeRecord(stream, record);
        return static_cast<bool>(stream);
    }

    class Recorder
    {
    public:
        Recorder()
        {
            if (const char* path = std::getenv("OPENMW_RUNTIME_DIAGNOSTICS_FILE"))
                mRuntime.open(std::filesystem::path(std::u8string(path, path + std::strlen(path))), std::ios::out | std::ios::trunc);
            if (const char* path = std::getenv("OPENMW_GAMEPLAY_DIAGNOSTICS_FILE"))
                mGameplay.open(std::filesystem::path(std::u8string(path, path + std::strlen(path))), std::ios::app);
            mQueues = std::make_unique<Queues>();
            mThread = std::thread([this] { run(); });
        }
        ~Recorder() { stop(); }
        Recorder(const Recorder&) = delete;
        Recorder& operator=(const Recorder&) = delete;
        void push(const Record& record) noexcept
        {
            if (mStopping.load(std::memory_order_relaxed)) { ++dropped; return; }
            // Bounded thread-assigned shards avoid worker/render contention.
            // More than eight producer threads share shards, with explicit loss
            // rather than blocking the engine when a shard is busy or full.
            static thread_local const std::size_t shard = mNextShard.fetch_add(1) % QueueShards;
            auto& queue = (*mQueues)[shard];
            std::unique_lock lock(queue.mutex, std::try_to_lock);
            if (!lock.owns_lock() || mStopping.load(std::memory_order_relaxed) || !queue.ring.push(record)) { ++dropped; mWake.notify_one(); return; }
            if (queue.ring.size() % 8 == 0) mWake.notify_one();
        }
        void stop() noexcept
        {
            mStopping.store(true, std::memory_order_relaxed);
            mWake.notify_one();
            if (mThread.joinable()) mThread.join();
        }
    private:
        struct Shard { std::mutex mutex; Ring<QueueCapacity / QueueShards> ring; };
        using Queues = std::array<Shard, QueueShards>;
        void run() noexcept
        {
            try
            {
                std::uint64_t nextProbe = 0, nextStats = 0, written = 0, writerUs = 0;
                std::array<bool, 2> capped{};
                for (;;)
                {
                    const auto start = nowUs();
                    if (!mStopping.load(std::memory_order_relaxed) && start >= nextProbe)
                    {
                        nextProbe = nowUs() + 1000000;
                        if (const auto probe = processProbe.load(std::memory_order_acquire)) probe();
                    }
                    std::array<Record, 8> batch;
                    std::size_t count = 0;
                    bool allEmpty = true;
                    for (std::size_t offset = 0; offset < QueueShards; ++offset)
                    {
                        auto& queue = (*mQueues)[(mReadShard + offset) % QueueShards];
                        std::unique_lock lock(queue.mutex);
                        while (count != batch.size() && queue.ring.pop(batch[count])) ++count;
                        allEmpty = allEmpty && queue.ring.size() == 0;
                    }
                    mReadShard = (mReadShard + 1) % QueueShards;
                    const bool done = mStopping.load(std::memory_order_relaxed) && allEmpty;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto lane = static_cast<std::size_t>(batch[i].legacy);
                        auto& stream = lane ? mGameplay : mRuntime;
                        if (writeBoundedRecord(stream, batch[i], capped[lane])) ++written;
                        else ++dropped;
                    }
                    if (mRuntime && !capped[0] && mRuntime.tellp() >= static_cast<std::streamoff>(FileLimit))
                    {
                        mRuntime << "{\"schema\":2,\"type\":\"capture_limit\",\"frame\":"
                            << currentFrame.load(std::memory_order_relaxed) << "}\n";
                        capped[0] = true;
                    }
                    if (mRuntime && (done || (!capped[0] && nowUs() >= nextStats)))
                    {
                        nextStats = nowUs() + 1000000;
                        mRuntime << "{\"schema\":2,\"type\":\"" << (done ? "recorder_end" : "recorder_stats")
                            << "\",\"time_us\":" << nowUs() << ",\"attempted\":" << attempted.load()
                            << ",\"written\":" << written << ",\"dropped\":" << dropped.load()
                            << ",\"clipped\":" << clipped.load() << ",\"producer_us\":" << producerUs.load()
                            << ",\"writer_us\":" << writerUs << ",\"queue_capacity\":" << QueueCapacity
                            << ",\"queue_bytes\":" << sizeof(Queues)
                            << ",\"runtime_bytes\":" << (mRuntime.tellp() >= 0 ? static_cast<std::uint64_t>(mRuntime.tellp()) : 0)
                            << ",\"gameplay_bytes\":" << (mGameplay && mGameplay.tellp() >= 0 ? static_cast<std::uint64_t>(mGameplay.tellp()) : 0)
                            << "}\n";
                    }
                    if (done)
                    {
                        mRuntime.flush(); mGameplay.flush();
                        break;
                    }
                    if (count == batch.size()) { writerUs += nowUs() - start; continue; }
                    mRuntime.flush(); mGameplay.flush();
                    writerUs += nowUs() - start; // Wall work including probe/format/flush; excludes idle sleep.
                    std::unique_lock lock(mWakeMutex);
                    if (!mStopping.load(std::memory_order_relaxed))
                        mWake.wait_for(lock, std::chrono::milliseconds(250));
                }
            }
            catch (...) { ++dropped; } // A recorder failure cannot fail the engine.
        }
        std::unique_ptr<Queues> mQueues;
        std::atomic<std::size_t> mNextShard{ 0 };
        std::size_t mReadShard = 0;
        std::mutex mWakeMutex;
        std::condition_variable mWake;
        std::atomic<bool> mStopping{ false };
        std::ofstream mRuntime, mGameplay;
        std::thread mThread;
    };
    inline Recorder* recorder() noexcept
    {
        if (!enabled()) return nullptr;
        static const auto instance = []() noexcept -> std::unique_ptr<Recorder> {
            try { return std::make_unique<Recorder>(); } catch (...) { return {}; }
        }();
        return instance.get();
    }
    using Fields = std::initializer_list<std::pair<std::string_view, std::uint64_t>>;
    inline void emit(std::string_view type, std::string_view owner, std::string_view identity, Fields fields = {}) noexcept
    {
        if (!enabled()) return;
        const auto start = nowUs();
        Record r;
        r.timeUs = start; r.frame = currentFrame.load(std::memory_order_relaxed);
        r.truncated = copyText(r.type, type) | copyText(r.owner, owner) | copyText(r.identity, identity);
        for (const auto& [key, value] : fields)
        {
            if (r.count == r.values.size()) { r.truncated = true; break; }
            auto& field = r.values[r.count++];
            r.truncated |= copyText(field.key, key); field.number = value;
        }
        ++attempted;
        if (r.truncated) ++clipped;
        if (auto* sink = recorder()) sink->push(r); else ++dropped;
        producerUs.fetch_add(nowUs() - start, std::memory_order_relaxed);
    }
    template <class TextFields>
    inline void legacy(std::uint64_t frame, std::string_view type, const TextFields& fields) noexcept
    {
        if (!enabled()) return;
        const auto start = nowUs();
        Record r;
        r.legacy = true; r.frame = frame; r.timeUs = start;
        r.truncated = copyText(r.type, type);
        for (const auto& [key, value] : fields)
        {
            if (r.count == r.values.size()) { r.truncated = true; break; }
            auto& field = r.values[r.count++];
            r.truncated |= copyText(field.key, key) | copyText(field.text, value);
        }
        ++attempted;
        if (r.truncated) ++clipped;
        if (auto* sink = recorder()) sink->push(r); else ++dropped;
        producerUs.fetch_add(nowUs() - start, std::memory_order_relaxed);
    }
    class Sampler
    {
    public:
        bool due(std::uint64_t intervalUs = 1000000) noexcept
        {
            if (!enabled()) return false;
            const auto now = nowUs();
            auto next = mNext.load(std::memory_order_relaxed);
            return now >= next && mNext.compare_exchange_strong(next, now + intervalUs, std::memory_order_relaxed);
        }
    private:
        std::atomic<std::uint64_t> mNext{ 0 };
    };
    class Operation
    {
    public:
        explicit Operation(std::string_view name, std::string_view identity = {}) noexcept
        {
            if (!enabled()) return;
            mStart = nowUs();
            mExceptions = std::uncaught_exceptions();
            copyText(mName, name); copyText(mIdentity, identity);
            static std::atomic<std::uint64_t> sequence{ 0 };
            mId = ++sequence;
            emit("work_begin", mName.data(), mIdentity.data(), {{"operation", mId}});
        }
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation()
        {
            if (mStart) emit("work_end", mName.data(), mIdentity.data(),
                {{"operation", mId}, {"elapsed_us", nowUs() - mStart},
                    {"unwinding", std::uncaught_exceptions() > mExceptions}});
        }
    private:
        std::array<char, 96> mName{};
        std::array<char, 256> mIdentity{};
        std::uint64_t mStart = 0, mId = 0;
        int mExceptions = 0;
    };
    struct PayloadInfo
    {
        const void* identity = nullptr;
        std::uint64_t bytes = 0, elements = 0;
        bool externallyReferenced = false;
    };
    // Partial, documented payload accounting for cache wrapper objects. This is
    // not an allocator replacement and never claims whole-process coverage.
    struct PayloadSource
    {
        virtual ~PayloadSource() = default;
        virtual PayloadInfo diagnosticPayload() const noexcept = 0;
    };
    template <class Vector>
    inline std::uint64_t capacityBytes(const Vector& v) noexcept
    {
        return static_cast<std::uint64_t>(v.capacity()) * sizeof(typename Vector::value_type);
    }
}
#endif
