#ifndef OPENMW_COMPONENTS_DEBUG_DIAGNOSTICTRANSPORT_H
#define OPENMW_COMPONENTS_DEBUG_DIAGNOSTICTRANSPORT_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace Debug::V3Diagnostics
{
    template <std::size_t Capacity> class DiagnosticByteBudget
    {
    public:
        bool claim(std::size_t bytes) noexcept
        {
            if (bytes > Capacity)
                return false;
            const auto prior = mBytes.fetch_add(bytes, std::memory_order_acq_rel);
            if (prior <= Capacity - bytes)
                return true;
            mBytes.fetch_sub(bytes, std::memory_order_release);
            return false;
        }
        void release(std::size_t bytes) noexcept { mBytes.fetch_sub(bytes, std::memory_order_release); }
        std::size_t pending() const noexcept { return mBytes.load(std::memory_order_acquire); }
    private:
        std::atomic<std::size_t> mBytes{ 0 };
    };

    // A bounded multi-producer/single-consumer ring. A producer claims one
    // sequence-numbered slot and publishes only after moving a complete owned
    // item into it. The consumer never reads an unpublished slot. No producer
    // takes the disk worker's mutex or waits for a different producer's slot.
    template <class Item, std::size_t Capacity> class DiagnosticMpscQueue
    {
        static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
        static_assert(std::is_nothrow_move_assignable_v<Item>);

    public:
        enum class PushResult { Published, Full, Contended };
        DiagnosticMpscQueue()
            : mSlots(std::make_unique<std::array<Slot, Capacity>>())
        {
            for (std::size_t i = 0; i < Capacity; ++i)
                (*mSlots)[i].sequence.store(i, std::memory_order_relaxed);
        }

        PushResult pushResult(Item&& item) noexcept
        {
            auto position = mProducer.load(std::memory_order_relaxed);
            // Bound instrumentation work even under adversarial contention.
            // Exhaustion is exposed as loss, just like a full ring.
            for (unsigned attempt = 0; attempt < 128; ++attempt)
            {
                auto& slot = (*mSlots)[position & (Capacity - 1)];
                const auto sequence = slot.sequence.load(std::memory_order_acquire);
                const auto difference = static_cast<std::intptr_t>(sequence - position);
                if (difference == 0)
                {
                    if (mProducer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                    {
                        slot.item = std::move(item);
                        slot.sequence.store(position + 1, std::memory_order_release);
                        return PushResult::Published;
                    }
                }
                else if (difference < 0)
                    return PushResult::Full;
                else
                    position = mProducer.load(std::memory_order_relaxed);
            }
            return PushResult::Contended;
        }

        bool push(Item&& item) noexcept { return pushResult(std::move(item)) == PushResult::Published; }

        bool pop(Item& item) noexcept
        {
            const auto position = mConsumer.load(std::memory_order_relaxed);
            auto& slot = (*mSlots)[position & (Capacity - 1)];
            if (slot.sequence.load(std::memory_order_acquire) != position + 1)
                return false;
            item = std::move(slot.item);
            slot.sequence.store(position + Capacity, std::memory_order_release);
            mConsumer.store(position + 1, std::memory_order_release);
            return true;
        }

        bool pending() const noexcept
        {
            return mProducer.load(std::memory_order_acquire) != mConsumer.load(std::memory_order_acquire);
        }

    private:
        struct Slot
        {
            std::atomic<std::size_t> sequence{ 0 };
            Item item{};
        };
        std::unique_ptr<std::array<Slot, Capacity>> mSlots;
        std::atomic<std::size_t> mProducer{ 0 };
        // Separate producer/consumer counters without imposing over-alignment
        // on the whole queue allocation on older toolchains.
        std::array<char, 64> mCursorSeparation{};
        std::atomic<std::size_t> mConsumer{ 0 };
    };

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
        std::atomic<std::size_t> mCapacityDropped{ 0 }, mContentionDropped{ 0 }, mOversizedDropped{ 0 };
        std::atomic<std::size_t> mAllocationDropped{ 0 }, mIoDropped{ 0 }, mStoppedDropped{ 0 };
        std::atomic<std::size_t> mPendingLines{ 0 };
        std::atomic<std::size_t> mActiveEnqueues{ 0 };
        std::atomic<bool> mCloseQueued{ false };
        std::atomic<bool> mFinished{ false };
        bool mOpenAttempted = false;
    };

    // Legacy V3 transport remains the default. The Phase 9 launcher selects the
    // independently controlled ring for all modes, preserving a common capture
    // transport across reference/candidates. Only the disk worker performs I/O.
    class DiagnosticWriterHub
    {
    public:
        using ChannelHandle = std::shared_ptr<DiagnosticChannel>;
        static constexpr std::size_t QueueCapacity = 32768;
        static constexpr std::size_t ByteCapacity = 32u * 1024u * 1024u;
        static constexpr std::size_t RowCapacity = 64u * 1024u;

        static DiagnosticWriterHub& instance()
        {
            static DiagnosticWriterHub hub(boundedRequested());
            return hub;
        }

        // Explicit construction also lets the native fixture exercise the
        // actual disk consumer and orderly finish without singleton teardown.
        explicit DiagnosticWriterHub(bool boundedTransport)
            : mBoundedTransport(boundedTransport)
        {
            if (mBoundedTransport)
            {
                try { mRing = std::make_unique<Ring>(); }
                catch (const std::bad_alloc&) { mInitializationFailed.store(true); }
            }
        }
        DiagnosticWriterHub(const DiagnosticWriterHub&) = delete;
        DiagnosticWriterHub& operator=(const DiagnosticWriterHub&) = delete;

        ChannelHandle registerChannel(std::string path, std::string header)
        {
            ChannelHandle channel;
            try
            {
                channel = std::make_shared<DiagnosticChannel>(std::move(path), std::move(header));
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStopping.load(std::memory_order_acquire))
                    return {};
                mChannels.push_back(channel);
                if (!mBoundedTransport)
                    mQueue.push_back({ channel, {}, true, false });
                if (!mWriterThread.joinable())
                    mWriterThread = std::thread([this] { writerEntry(); });
            }
            catch (...)
            {
                // Missing requested output is invalid evidence. Do not crash
                // gameplay if diagnostic allocation or worker startup fails.
                mInitializationFailed.store(true, std::memory_order_release);
                return {};
            }
            mCondition.notify_one();
            return channel;
        }

        void enqueue(const ChannelHandle& channel, const std::string& line)
        {
            if (!channel)
                return;
            if (!mBoundedTransport)
            {
                if (channel->mCloseQueued.load(std::memory_order_acquire))
                    return;
                std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
                if (!lock.owns_lock() || mStopping.load(std::memory_order_relaxed) || mQueue.size() >= 16384)
                {
                    channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                mQueue.push_back({ channel, line, false, false });
                lock.unlock();
                mCondition.notify_one();
                return;
            }

            // close is an end-of-capture boundary: later submissions are not
            // part of that capture. Accepted pre-close rows are drained below.
            if (channel->mCloseQueued.load(std::memory_order_acquire))
                return;
            channel->mActiveEnqueues.fetch_add(1, std::memory_order_acq_rel);
            const auto activeGuard = [&] { channel->mActiveEnqueues.fetch_sub(1, std::memory_order_release); };
            if (channel->mCloseQueued.load(std::memory_order_acquire))
            {
                activeGuard();
                return;
            }
            if (mStopping.load(std::memory_order_acquire) || !mRing || mInitializationFailed.load(std::memory_order_acquire)
                || line.size() > RowCapacity)
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                if (line.size() > RowCapacity)
                    channel->mOversizedDropped.fetch_add(1, std::memory_order_relaxed);
                else if (!mRing || mInitializationFailed.load())
                    channel->mAllocationDropped.fetch_add(1, std::memory_order_relaxed);
                else
                    channel->mStoppedDropped.fetch_add(1, std::memory_order_relaxed);
                activeGuard();
                return;
            }
            const auto bytes = line.size();
            if (!mQueuedBytes.claim(bytes))
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                channel->mCapacityDropped.fetch_add(1, std::memory_order_relaxed);
                activeGuard();
                return;
            }

            bool published = false;
            try
            {
                QueueItem item{ channel, line, false, false };
                channel->mPendingLines.fetch_add(1, std::memory_order_release);
                const auto result = mRing->pushResult(std::move(item));
                published = result == Ring::PushResult::Published;
                if (!published)
                {
                    channel->mPendingLines.fetch_sub(1, std::memory_order_release);
                    if (result == Ring::PushResult::Full)
                        channel->mCapacityDropped.fetch_add(1, std::memory_order_relaxed);
                    else
                        channel->mContentionDropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            catch (...)
            {
                // Allocation failure must invalidate a capture, not crash the
                // game or leave an unpublished reservation blocking the ring.
                channel->mAllocationDropped.fetch_add(1, std::memory_order_relaxed);
            }
            if (!published)
            {
                mQueuedBytes.release(bytes);
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
            }
            activeGuard();
            if (published)
                mCondition.notify_one();
        }

        void closeChannel(const ChannelHandle& channel)
        {
            if (!channel || channel->mCloseQueued.exchange(true, std::memory_order_acq_rel))
                return;
            if (!mBoundedTransport)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mStopping.load(std::memory_order_relaxed))
                    mQueue.push_back({ channel, {}, false, true });
            }
            mCondition.notify_one();
        }

        // Called after producers have stopped; this is an orderly-shutdown
        // operation, never a render-thread flush or a per-frame rendezvous.
        void finish()
        {
            mStopping.store(true, std::memory_order_release);
            mCondition.notify_one();
            if (mWriterThread.joinable())
                mWriterThread.join();
            if (mBoundedTransport)
            {
                // Worker/path/stream allocation failure must not terminate the
                // game. Accepted unfinished rows remain explicitly lost, and
                // best-effort finalization announces failed initialization.
                for (const auto& channel : mChannels)
                    if (channel)
                    {
                        if (mInitializationFailed.load(std::memory_order_acquire))
                        {
                            const auto unfinished = channel->mPendingLines.exchange(0);
                            channel->mDroppedLines.fetch_add(unfinished, std::memory_order_relaxed);
                            channel->mAllocationDropped.fetch_add(unfinished, std::memory_order_relaxed);
                        }
                        try { finalizeChannel(*channel); }
                        catch (...) { mInitializationFailed.store(true, std::memory_order_release); }
                    }
            }
            else if (!mWriterThread.joinable())
                for (const auto& channel : mChannels)
                    if (channel)
                        finalizeChannel(*channel);
        }

        ~DiagnosticWriterHub() { finish(); }

    private:
        struct QueueItem
        {
            ChannelHandle mChannel;
            std::string mLine;
            bool mOpenOnly = false;
            bool mClose = false;
        };
        using Ring = DiagnosticMpscQueue<QueueItem, QueueCapacity>;

        static bool boundedRequested()
        {
            const char* value = std::getenv("OPENMW_P9_CAPTURE_TRANSPORT");
            return value && std::string_view(value) == "1";
        }

        static void openChannel(DiagnosticChannel& channel)
        {
            if (channel.mOpenAttempted)
                return;
            channel.mOpenAttempted = true;
            channel.mStream.open(std::filesystem::u8path(channel.mPath), std::ios::out | std::ios::trunc);
            if (channel.mStream.is_open())
                channel.mStream << channel.mHeader << '\n';
        }

        void finalizeChannel(DiagnosticChannel& channel)
        {
            if (channel.mFinished.load(std::memory_order_relaxed))
                return;
            openChannel(channel);
            if (channel.mStream.is_open())
            {
                const auto dropped = channel.mDroppedLines.load(std::memory_order_relaxed);
                if (dropped > 0 || mBoundedTransport)
                    channel.mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\n';
                if (mBoundedTransport)
                    channel.mStream << "# p9_capture_transport=bounded_mpsc_v1\n"
                        << "# p9_capture_queue_items=" << QueueCapacity << '\n'
                        << "# p9_capture_queue_bytes=" << ByteCapacity << '\n'
                        << "# p9_capture_row_bytes=" << RowCapacity << '\n';
                channel.mStream.flush();
                const bool flushed = static_cast<bool>(channel.mStream);
                channel.mStream.close();
                const bool outputOk = flushed && static_cast<bool>(channel.mStream) && !mInitializationFailed.load();
                if (mBoundedTransport)
                {
                    // Independent sidecar is fail-closed when the CSV cannot
                    // be opened, written or flushed. A footer alone cannot
                    // announce that the file containing it failed to flush.
                    std::ofstream status(channel.mPath + ".writer-status.txt");
                    status << "normal_finish=1\noutput_ok=" << outputOk
                        << "\nrows_dropped=" << dropped << "\nqueue_capacity=" << QueueCapacity
                        << "\nbyte_capacity=" << ByteCapacity << "\nrow_capacity=" << RowCapacity
                        << "\ncapacity_dropped=" << channel.mCapacityDropped.load()
                        << "\ncontention_dropped=" << channel.mContentionDropped.load()
                        << "\noversized_dropped=" << channel.mOversizedDropped.load()
                        << "\nallocation_dropped=" << channel.mAllocationDropped.load()
                        << "\nio_dropped=" << channel.mIoDropped.load()
                        << "\nstopped_dropped=" << channel.mStoppedDropped.load()
                        << "\ninitialization_failed=" << mInitializationFailed.load() << '\n';
                }
            }
            else if (mBoundedTransport)
            {
                std::ofstream status(channel.mPath + ".writer-status.txt");
                status << "normal_finish=1\noutput_ok=0\nrows_dropped="
                    << channel.mDroppedLines.load(std::memory_order_relaxed) << '\n';
            }
            channel.mFinished.store(true, std::memory_order_release);
        }

        void consume(QueueItem& item)
        {
            if (!item.mChannel)
                return;
            auto& channel = *item.mChannel;
            if (item.mClose)
                finalizeChannel(channel);
            else
            {
                openChannel(channel);
                if (!item.mOpenOnly)
                {
                    if (channel.mStream.is_open())
                        channel.mStream << item.mLine << '\n';
                    if (!channel.mStream)
                    {
                        channel.mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                        channel.mIoDropped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        void writerLoop()
        {
            std::deque<QueueItem> local;
            for (;;)
            {
                if (mBoundedTransport)
                {
                    QueueItem item;
                    while (mRing && mRing->pop(item))
                    {
                        const auto bytes = item.mLine.size();
                        consume(item);
                        mQueuedBytes.release(bytes);
                        item.mChannel->mPendingLines.fetch_sub(1, std::memory_order_release);
                        item = {};
                    }
                    std::unique_lock<std::mutex> lock(mMutex);
                    for (const auto& channel : mChannels)
                        if (channel->mCloseQueued.load(std::memory_order_acquire)
                            && channel->mActiveEnqueues.load(std::memory_order_acquire) == 0
                            && channel->mPendingLines.load(std::memory_order_acquire) == 0)
                            finalizeChannel(*channel);
                    if (mStopping.load(std::memory_order_acquire) && (!mRing || !mRing->pending()))
                        break;
                    // Producers do not take this mutex. A bounded timed wait
                    // closes the notify-before-wait race without blocking them;
                    // it performs no periodic stream flush.
                    mCondition.wait_for(lock, std::chrono::milliseconds(10));
                }
                else
                {
                    {
                        std::unique_lock<std::mutex> lock(mMutex);
                        mCondition.wait(lock, [this] { return mStopping.load() || !mQueue.empty(); });
                        if (mQueue.empty() && mStopping.load())
                            break;
                        local.swap(mQueue);
                    }
                    while (!local.empty())
                    {
                        auto item = std::move(local.front());
                        local.pop_front();
                        consume(item);
                    }
                }
            }
            for (const auto& channel : mChannels)
                if (channel)
                    finalizeChannel(*channel);
        }

        void writerEntry()
        {
            if (!mBoundedTransport)
            {
                writerLoop();
                return;
            }
            try { writerLoop(); }
            catch (...)
            {
                // No producer-thread output is substituted after a worker
                // failure. finish() can attempt a failed sidecar after all
                // producers stop; missing/partial status is invalid evidence.
                mInitializationFailed.store(true, std::memory_order_release);
            }
        }

        const bool mBoundedTransport;
        std::unique_ptr<Ring> mRing;
        DiagnosticByteBudget<ByteCapacity> mQueuedBytes;
        std::atomic<bool> mStopping{ false };
        std::atomic<bool> mInitializationFailed{ false };
        std::mutex mMutex;
        std::condition_variable mCondition;
        std::deque<QueueItem> mQueue;
        std::vector<ChannelHandle> mChannels;
        std::thread mWriterThread;
    };
}

#endif
