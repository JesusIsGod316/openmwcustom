#ifndef OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H
#define OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

    inline std::size_t threadId()
    {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
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

    struct DiagnosticChannel
    {
        DiagnosticChannel(std::string path, std::string header)
            : mPath(std::move(path))
            , mHeader(std::move(header))
        {
        }

        std::string mPath;
        std::string mHeader;
        std::ofstream mStream;
        std::atomic<std::size_t> mDroppedLines{ 0 };
        std::atomic<bool> mCloseQueued{ false };
        bool mOpenAttempted = false;
    };

    // V3.17: all V3 CsvWriter instances share one bounded, nonblocking producer
    // queue and one disk writer. V3.16 removed direct producer-thread I/O but
    // still created one writer thread per enabled CSV and periodically flushed
    // each stream. The shared hub eliminates that thread/flush periodicity.
    class DiagnosticWriterHub
    {
    public:
        using ChannelHandle = std::shared_ptr<DiagnosticChannel>;

        static DiagnosticWriterHub& instance()
        {
            static DiagnosticWriterHub hub;
            return hub;
        }

        DiagnosticWriterHub(const DiagnosticWriterHub&) = delete;
        DiagnosticWriterHub& operator=(const DiagnosticWriterHub&) = delete;

        ChannelHandle registerChannel(std::string path, std::string header)
        {
            auto channel = std::make_shared<DiagnosticChannel>(std::move(path), std::move(header));
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStopping)
                    return {};
                mChannels.push_back(channel);
                // Queue an open/header operation so even an enabled stream with
                // no data rows is materialized without producer-thread file I/O.
                mQueue.push_back({ channel, {}, true, false });
                if (!mWriterThread.joinable())
                    mWriterThread = std::thread([this] { writerLoop(); });
            }
            mCondition.notify_one();
            return channel;
        }

        void enqueue(const ChannelHandle& channel, const std::string& line)
        {
            if (!channel || channel->mCloseQueued.load(std::memory_order_acquire))
                return;

            // Instrumentation must never wait behind its own writer. A dropped
            // row is less harmful than contaminating the frametime being measured.
            std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (mStopping || mQueue.size() >= sMaxQueuedItems)
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            mQueue.push_back({ channel, line, false, false });
            lock.unlock();
            mCondition.notify_one();
        }

        void closeChannel(const ChannelHandle& channel)
        {
            if (!channel || channel->mCloseQueued.exchange(true, std::memory_order_acq_rel))
                return;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStopping)
                    return;
                mQueue.push_back({ channel, {}, false, true });
            }
            mCondition.notify_one();
        }

        ~DiagnosticWriterHub()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStopping = true;
            }
            mCondition.notify_one();
            if (mWriterThread.joinable())
                mWriterThread.join();
        }

    private:
        struct QueueItem
        {
            ChannelHandle mChannel;
            std::string mLine;
            bool mOpenOnly = false;
            bool mClose = false;
        };

        DiagnosticWriterHub() = default;

        static void openChannel(DiagnosticChannel& channel)
        {
            if (channel.mOpenAttempted)
                return;
            channel.mOpenAttempted = true;
            channel.mStream.open(std::filesystem::u8path(channel.mPath), std::ios::out | std::ios::trunc);
            if (channel.mStream.is_open())
                channel.mStream << channel.mHeader << '\n';
        }

        static void finalizeChannel(DiagnosticChannel& channel)
        {
            openChannel(channel);
            if (!channel.mStream.is_open())
                return;
            const std::size_t dropped = channel.mDroppedLines.load(std::memory_order_relaxed);
            if (dropped > 0)
                channel.mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\n';
            channel.mStream.flush();
            channel.mStream.close();
        }

        void writerLoop()
        {
            std::deque<QueueItem> local;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCondition.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                    if (mQueue.empty() && mStopping)
                        break;
                    local.swap(mQueue);
                }

                while (!local.empty())
                {
                    QueueItem item = std::move(local.front());
                    local.pop_front();
                    if (!item.mChannel)
                        continue;
                    if (item.mClose)
                    {
                        finalizeChannel(*item.mChannel);
                        continue;
                    }
                    openChannel(*item.mChannel);
                    if (!item.mOpenOnly && item.mChannel->mStream.is_open())
                        item.mChannel->mStream << item.mLine << '\n';
                }
            }

            // No gameplay-cadence flushes. Drain first, then record losses and
            // flush each enabled stream exactly once during orderly shutdown.
            for (const ChannelHandle& channel : mChannels)
            {
                if (!channel)
                    continue;
                if (!channel->mCloseQueued.load(std::memory_order_acquire))
                    finalizeChannel(*channel);
            }
        }

        static constexpr std::size_t sMaxQueuedItems = 16384;

        std::mutex mMutex;
        std::condition_variable mCondition;
        std::deque<QueueItem> mQueue;
        std::vector<ChannelHandle> mChannels;
        std::thread mWriterThread;
        bool mStopping = false;
    };

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
            return mEnabled.load(std::memory_order_acquire);
        }

        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mEnabled.load(std::memory_order_acquire))
                return;
            DiagnosticWriterHub::instance().enqueue(mChannel, line);
        }

    private:
        void ensureOpen()
        {
            if (mAttempted.load(std::memory_order_acquire))
                return;

            std::lock_guard<std::mutex> lock(mInitMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                mPath = raw;
                mChannel = DiagnosticWriterHub::instance().registerChannel(mPath, mHeader);
                mEnabled.store(static_cast<bool>(mChannel), std::memory_order_release);
            }

            mAttempted.store(true, std::memory_order_release);
        }

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        DiagnosticWriterHub::ChannelHandle mChannel;
        std::mutex mInitMutex;
        std::atomic<bool> mAttempted{ false };
        std::atomic<bool> mEnabled{ false };
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

    // Generic opt-in scope timer used by the Optimization Lab. The destination
    // writer decides which environment variable enables the stream. A minimum
    // duration keeps hot-path diagnostic files manageable.
    class ScopedCsvTimer
    {
    public:
        ScopedCsvTimer(CsvWriter& writer, std::string_view phase, std::string_view detail = {}, double minimumMs = 0.0)
            : mWriter(writer)
            , mEnabled(writer.enabled())
            , mMinimumMs(minimumMs)
            , mPhase(mEnabled ? phase : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
            , mStart(mEnabled ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScopedCsvTimer()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << csvQuote(mPhase) << ','
                << csvQuote(mDetail) << ',' << std::fixed << std::setprecision(3) << durationMs;
            mWriter.writeLine(row.str());
        }

        ScopedCsvTimer(const ScopedCsvTimer&) = delete;
        ScopedCsvTimer& operator=(const ScopedCsvTimer&) = delete;

    private:
        CsvWriter& mWriter;
        bool mEnabled;
        double mMinimumMs;
        std::string mPhase;
        std::string mDetail;
        Clock::time_point mStart;
    };

    inline CsvWriter& transitionWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRANSITION_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& pagingWriter()
    {
        static CsvWriter writer("OPENMW_V3_PAGING_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& resourceWriter()
    {
        static CsvWriter writer("OPENMW_V3_RESOURCE_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& navWriter()
    {
        static CsvWriter writer("OPENMW_V3_NAV_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& insertionWriter()
    {
        static CsvWriter writer("OPENMW_V3_INSERT_FILE",
            "frame,epoch_ms,cell,total_refs,rendered_refs,physics_refs,actors,animated,doors,render_ms,mechanics_ms,"
            "particles_ms,physics_ms,lua_added_ms,nav_ms");
        return writer;
    }

    inline CsvWriter& v32RendererInsertionWriter()
    {
        static CsvWriter writer("OPENMW_V32_RENDER_INSERT_FILE",
            "frame,epoch_ms,cell,objects,constructed,restored,paged,static_refs,animated_refs,actors,lights,"
            "renderer_total_ms,mean_object_ms,scene_instance_ms,object_root_exclusive_ms,controller_setup_ms,"
            "transform_attach_ms,misc_ms,max_object_ms,max_ref,max_model");
        return writer;
    }

    inline CsvWriter& workQueueWriter()
    {
        static CsvWriter writer("OPENMW_V3_WORKQUEUE_FILE",
            "frame,epoch_ms,thread,event,item,type,queue_depth,active_threads,duration_ms");
        return writer;
    }

    inline CsvWriter& renderWriter()
    {
        static CsvWriter writer("OPENMW_V3_RENDER_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& postFxWriter()
    {
        static CsvWriter writer("OPENMW_V3_POSTFX_FILE",
            "frame,epoch_ms,thread,technique,pass,cpu_submit_ms,width,height,render_target,mipmap");
        return writer;
    }

    inline CsvWriter& streamingWriter()
    {
        static CsvWriter writer("OPENMW_V3_STREAMING_FILE",
            "frame,epoch_ms,event,category,detail,last_frame_ms,limit,count");
        return writer;
    }

    inline CsvWriter& gpuMemoryWriter()
    {
        static CsvWriter writer("OPENMW_V32_GPU_MEMORY_FILE",
            "frame,epoch_ms,dedicated_usage_mb,dedicated_budget_mb,available_for_reservation_mb,"
            "current_reservation_mb,budget_used_pct,effective_soft_mb,effective_hard_mb,pressure,"
            "nvml_available,adapter_used_mb,adapter_free_mb,adapter_total_mb");
        return writer;
    }

    inline CsvWriter& traceWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRACE_FILE",
            "frame,epoch_ms,thread,id,parent,category,name,detail,duration_ms");
        return writer;
    }

    // Nested, cross-thread trace scope. IDs and parent IDs let an offline tool
    // reconstruct the critical path instead of correlating unrelated CSV rows.
    class TraceScope
    {
    public:
        TraceScope(std::string_view category, std::string_view name, std::string_view detail = {}, double minimumMs = 0.0)
            : mEnabled(traceWriter().enabled())
            , mMinimumMs(minimumMs)
            , mCategory(mEnabled ? category : std::string_view{})
            , mName(mEnabled ? name : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
        {
            if (!mEnabled)
                return;
            mId = sNextId.fetch_add(1, std::memory_order_relaxed);
            mParent = sCurrentParent;
            sCurrentParent = mId;
            mStart = Clock::now();
        }

        ~TraceScope()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            sCurrentParent = mParent;
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << threadId() << ',' << mId << ','
                << mParent << ',' << csvQuote(mCategory) << ',' << csvQuote(mName) << ',' << csvQuote(mDetail) << ','
                << std::fixed << std::setprecision(3) << durationMs;
            traceWriter().writeLine(row.str());
        }

        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;

    private:
        inline static std::atomic<unsigned long long> sNextId{ 1 };
        inline static thread_local unsigned long long sCurrentParent = 0;

        bool mEnabled = false;
        double mMinimumMs = 0.0;
        std::string mCategory;
        std::string mName;
        std::string mDetail;
        unsigned long long mId = 0;
        unsigned long long mParent = 0;
        Clock::time_point mStart{};
    };

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
