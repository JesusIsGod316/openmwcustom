#ifndef OPENMW_COMPONENTS_SCENEUTIL_FRAMECRITICALJOBGROUP_H
#define OPENMW_COMPONENTS_SCENEUTIL_FRAMECRITICALJOBGROUP_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace SceneUtil
{
    // V3.25 bridge primitive: synchronous fork/join work for frame-critical kernels.
    //
    // This is intentionally separate from V3.24 FrameJobService. Existing Critical
    // and Opportunistic lane semantics remain unchanged. Workers are created lazily
    // only when a caller actually requests parallel work, so V3.25 controls that do
    // not enable CP2 do not gain extra resident worker threads.
    //
    // There is no task FIFO. One caller owns one group immediately, publishes a
    // frozen range task to reserved workers, participates in the same atomic range
    // cursor, then waits only for those workers. A concurrent/nested group fails
    // closed to caller-only execution rather than queueing behind another group.
    class FrameCriticalJobGroup
    {
    public:
        using RangeTask = std::function<void(std::size_t begin, std::size_t end)>;

        struct Stats
        {
            std::uint64_t groups = 0;
            std::uint64_t items = 0;
            std::uint64_t workerChunks = 0;
            std::uint64_t workerItems = 0;
            std::uint64_t callerChunks = 0;
            std::uint64_t callerItems = 0;
            std::uint64_t fallbackSerialGroups = 0;
            std::uint64_t failedGroups = 0;
            std::uint64_t peakWorkers = 0;
        };

        static FrameCriticalJobGroup& instance()
        {
            static FrameCriticalJobGroup group;
            return group;
        }

        FrameCriticalJobGroup(const FrameCriticalJobGroup&) = delete;
        FrameCriticalJobGroup& operator=(const FrameCriticalJobGroup&) = delete;

        bool parallelFor(
            std::size_t itemCount, std::size_t requestedWorkers, std::size_t chunkSize, RangeTask task)
        {
            if (itemCount == 0 || !task)
                return true;

            chunkSize = std::max<std::size_t>(1, chunkSize);
            const std::size_t chunkCount = (itemCount + chunkSize - 1) / chunkSize;
            requestedWorkers
                = std::min(requestedWorkers, chunkCount > 1 ? chunkCount - 1 : std::size_t{ 0 });

            if (requestedWorkers == 0)
                return runInline(itemCount, std::move(task), false);

            std::unique_lock submitLock(mSubmitMutex, std::try_to_lock);
            if (!submitLock.owns_lock())
            {
                mFallbackSerialGroups.fetch_add(1, std::memory_order_relaxed);
                return runInline(itemCount, std::move(task), true);
            }

            ensureWorkers(requestedWorkers);

            RangeTask callerTask;
            std::uint64_t generation = 0;
            {
                std::lock_guard stateLock(mStateMutex);
                mTask = std::move(task);
                mItemCount = itemCount;
                mChunkSize = chunkSize;
                mRequestedWorkers = requestedWorkers;
                mCompletedWorkers = 0;
                mNext.store(0, std::memory_order_release);
                mFailed.store(false, std::memory_order_release);
                generation = ++mGeneration;
                callerTask = mTask;
            }

            mGroups.fetch_add(1, std::memory_order_relaxed);
            mItems.fetch_add(itemCount, std::memory_order_relaxed);
            mWork.notify_all();

            consume(callerTask, itemCount, chunkSize, false);

            bool success = true;
            {
                std::unique_lock stateLock(mStateMutex);
                mDone.wait(stateLock, [this, generation, requestedWorkers] {
                    return mStop || (mGeneration == generation && mCompletedWorkers >= requestedWorkers);
                });
                success = !mStop && !mFailed.load(std::memory_order_acquire);
                mTask = {};
                mRequestedWorkers = 0;
            }

            if (!success)
                mFailedGroups.fetch_add(1, std::memory_order_relaxed);
            return success;
        }

        Stats stats() const
        {
            return Stats{
                mGroups.load(std::memory_order_relaxed),
                mItems.load(std::memory_order_relaxed),
                mWorkerChunks.load(std::memory_order_relaxed),
                mWorkerItems.load(std::memory_order_relaxed),
                mCallerChunks.load(std::memory_order_relaxed),
                mCallerItems.load(std::memory_order_relaxed),
                mFallbackSerialGroups.load(std::memory_order_relaxed),
                mFailedGroups.load(std::memory_order_relaxed),
                mPeakWorkers.load(std::memory_order_relaxed),
            };
        }

    private:
        FrameCriticalJobGroup() = default;

        ~FrameCriticalJobGroup()
        {
            {
                std::lock_guard lock(mStateMutex);
                mStop = true;
            }
            mWork.notify_all();
            mDone.notify_all();
            for (std::thread& worker : mWorkers)
                if (worker.joinable())
                    worker.join();

            writeSummary();
        }

        bool runInline(std::size_t itemCount, RangeTask task, bool fallback)
        {
            try
            {
                task(0, itemCount);
                mCallerChunks.fetch_add(1, std::memory_order_relaxed);
                mCallerItems.fetch_add(itemCount, std::memory_order_relaxed);
                return true;
            }
            catch (...)
            {
                if (fallback)
                    mFailedGroups.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        void ensureWorkers(std::size_t requestedWorkers)
        {
            while (mWorkers.size() < requestedWorkers)
            {
                const std::size_t index = mWorkers.size();
                mWorkers.emplace_back([this, index] { workerLoop(index); });
            }
            const auto workerCount = static_cast<std::uint64_t>(mWorkers.size());
            const auto oldPeak = mPeakWorkers.load(std::memory_order_relaxed);
            if (workerCount > oldPeak)
                mPeakWorkers.store(workerCount, std::memory_order_relaxed);
        }

        void workerLoop(std::size_t index)
        {
            std::uint64_t seenGeneration = 0;
            while (true)
            {
                RangeTask task;
                std::size_t itemCount = 0;
                std::size_t chunkSize = 1;
                std::uint64_t generation = 0;
                {
                    std::unique_lock lock(mStateMutex);
                    mWork.wait(lock, [this, index, seenGeneration] {
                        return mStop || (mGeneration != seenGeneration && index < mRequestedWorkers);
                    });
                    if (mStop)
                        return;

                    generation = mGeneration;
                    seenGeneration = generation;
                    task = mTask;
                    itemCount = mItemCount;
                    chunkSize = mChunkSize;
                }

                consume(task, itemCount, chunkSize, true);

                {
                    std::lock_guard lock(mStateMutex);
                    if (generation == mGeneration && index < mRequestedWorkers)
                        ++mCompletedWorkers;
                }
                mDone.notify_one();
            }
        }

        void consume(const RangeTask& task, std::size_t itemCount, std::size_t chunkSize, bool worker)
        {
            while (!mFailed.load(std::memory_order_acquire))
            {
                const std::size_t begin = mNext.fetch_add(chunkSize, std::memory_order_acq_rel);
                if (begin >= itemCount)
                    return;
                const std::size_t end = std::min(itemCount, begin + chunkSize);

                try
                {
                    task(begin, end);
                }
                catch (...)
                {
                    mFailed.store(true, std::memory_order_release);
                    mNext.store(itemCount, std::memory_order_release);
                    return;
                }

                if (worker)
                {
                    mWorkerChunks.fetch_add(1, std::memory_order_relaxed);
                    mWorkerItems.fetch_add(end - begin, std::memory_order_relaxed);
                }
                else
                {
                    mCallerChunks.fetch_add(1, std::memory_order_relaxed);
                    mCallerItems.fetch_add(end - begin, std::memory_order_relaxed);
                }
            }
        }

        void writeSummary() const
        {
            const char* path = std::getenv("OPENMW_V325_JOBGROUP_STATS_FILE");
            if (path == nullptr || path[0] == '\0')
                return;

            const Stats value = stats();
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out)
                return;
            out << "groups,items,worker_chunks,worker_items,caller_chunks,caller_items,"
                   "fallback_serial_groups,failed_groups,peak_workers\n";
            out << value.groups << ',' << value.items << ',' << value.workerChunks << ',' << value.workerItems << ','
                << value.callerChunks << ',' << value.callerItems << ',' << value.fallbackSerialGroups << ','
                << value.failedGroups << ',' << value.peakWorkers << '\n';
        }

        std::mutex mSubmitMutex;
        mutable std::mutex mStateMutex;
        std::condition_variable mWork;
        std::condition_variable mDone;
        std::vector<std::thread> mWorkers;
        RangeTask mTask;
        std::size_t mItemCount = 0;
        std::size_t mChunkSize = 1;
        std::size_t mRequestedWorkers = 0;
        std::size_t mCompletedWorkers = 0;
        std::uint64_t mGeneration = 0;
        std::atomic<std::size_t> mNext{ 0 };
        std::atomic_bool mFailed{ false };
        bool mStop = false;

        std::atomic<std::uint64_t> mGroups{ 0 };
        std::atomic<std::uint64_t> mItems{ 0 };
        std::atomic<std::uint64_t> mWorkerChunks{ 0 };
        std::atomic<std::uint64_t> mWorkerItems{ 0 };
        std::atomic<std::uint64_t> mCallerChunks{ 0 };
        std::atomic<std::uint64_t> mCallerItems{ 0 };
        std::atomic<std::uint64_t> mFallbackSerialGroups{ 0 };
        std::atomic<std::uint64_t> mFailedGroups{ 0 };
        std::atomic<std::uint64_t> mPeakWorkers{ 0 };
    };
}

#endif
