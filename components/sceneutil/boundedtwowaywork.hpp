#ifndef OPENMW_COMPONENTS_SCENEUTIL_BOUNDEDTWOWAYWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_BOUNDEDTWOWAYWORK_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace SceneUtil
{
    // Persistent bounded helper pool for coarse thread-safe work.
    // - caller always owns partition 0
    // - up to three persistent helper threads own partitions 1..N
    // - one operation engine-wide at a time; contention fails open to serial
    // - no FIFO/background queue and no borrowed task survives run()
    class BoundedTwoWayWork final
    {
    public:
        using RangeTask = std::function<void(std::size_t begin, std::size_t end, bool helper)>;
        using IndexedRangeTask
            = std::function<void(std::size_t begin, std::size_t end, std::size_t workerIndex)>;

        // Backward-compatible proven P3B one-helper API.
        static bool run(std::size_t count, std::size_t minimumCount, RangeTask task)
        {
            if (!task)
                return false;
            const std::size_t helpers = runIndexed(count, minimumCount, 1,
                [task = std::move(task)](std::size_t begin, std::size_t end, std::size_t workerIndex) {
                    task(begin, end, workerIndex != 0);
                });
            return helpers != 0;
        }

        // Returns the number of helper threads actually used. Zero means the
        // caller must execute the serial fallback.
        static std::size_t runIndexed(
            std::size_t count, std::size_t minimumCount, std::size_t requestedHelpers, IndexedRangeTask task)
        {
            return instance().runImpl(
                count, minimumCount, std::clamp(requestedHelpers, std::size_t{ 1 }, std::size_t{ 3 }), std::move(task));
        }

    private:
        static BoundedTwoWayWork& instance()
        {
            static BoundedTwoWayWork value;
            return value;
        }

        BoundedTwoWayWork() = default;

        ~BoundedTwoWayWork()
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
        }

        std::size_t ensureWorkers(std::size_t requested)
        {
            while (mWorkers.size() < requested)
            {
                const std::size_t workerIndex = mWorkers.size() + 1;
                try
                {
                    mWorkers.emplace_back([this, workerIndex] { workerLoop(workerIndex); });
                }
                catch (const std::system_error&)
                {
                    break;
                }
            }
            return std::min(requested, mWorkers.size());
        }

        std::size_t runImpl(
            std::size_t count, std::size_t minimumCount, std::size_t requestedHelpers, IndexedRangeTask task)
        {
            if (!task || count < (std::max)(std::size_t{ 2 }, minimumCount))
                return 0;

            std::unique_lock gate(mGateMutex, std::try_to_lock);
            if (!gate.owns_lock())
                return 0;

            const std::size_t helpers = ensureWorkers(requestedHelpers);
            if (helpers == 0)
                return 0;

            const std::size_t partitions = helpers + 1;
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(mStateMutex);
                mTask = std::move(task);
                mCount = count;
                mActiveHelpers = helpers;
                mPending = helpers;
                mHelperError = nullptr;
                generation = ++mGeneration;
            }
            mWork.notify_all();

            std::exception_ptr callerError;
            try
            {
                const std::size_t end = count / partitions;
                mTask(0, end, 0);
            }
            catch (...)
            {
                callerError = std::current_exception();
            }

            std::exception_ptr helperError;
            {
                std::unique_lock lock(mStateMutex);
                mDone.wait(lock, [this, generation] {
                    return mStop || (mCompletedGeneration >= generation && mPending == 0);
                });
                helperError = mHelperError;
                mTask = {};
            }

            if (callerError)
                std::rethrow_exception(callerError);
            if (helperError)
                std::rethrow_exception(helperError);
            return mStop ? 0 : helpers;
        }

        void workerLoop(std::size_t workerIndex)
        {
            std::uint64_t seenGeneration = 0;
            for (;;)
            {
                IndexedRangeTask task;
                std::size_t begin = 0;
                std::size_t end = 0;
                std::uint64_t generation = 0;
                bool active = false;
                {
                    std::unique_lock lock(mStateMutex);
                    mWork.wait(lock, [this, seenGeneration] {
                        return mStop || mGeneration != seenGeneration;
                    });
                    if (mStop)
                        return;

                    generation = mGeneration;
                    seenGeneration = generation;
                    active = workerIndex <= mActiveHelpers;
                    if (active)
                    {
                        task = mTask;
                        const std::size_t partitions = mActiveHelpers + 1;
                        begin = mCount * workerIndex / partitions;
                        end = mCount * (workerIndex + 1) / partitions;
                    }
                }

                if (!active)
                    continue;

                std::exception_ptr error;
                try
                {
                    task(begin, end, workerIndex);
                }
                catch (...)
                {
                    error = std::current_exception();
                }

                {
                    std::lock_guard lock(mStateMutex);
                    if (generation == mGeneration)
                    {
                        if (error && !mHelperError)
                            mHelperError = error;
                        if (mPending > 0)
                            --mPending;
                        if (mPending == 0)
                            mCompletedGeneration = generation;
                    }
                }
                mDone.notify_one();
            }
        }

        std::mutex mGateMutex;
        std::mutex mStateMutex;
        std::condition_variable mWork;
        std::condition_variable mDone;
        std::vector<std::thread> mWorkers;
        IndexedRangeTask mTask;
        std::size_t mCount = 0;
        std::size_t mActiveHelpers = 0;
        std::size_t mPending = 0;
        std::uint64_t mGeneration = 0;
        std::uint64_t mCompletedGeneration = 0;
        std::exception_ptr mHelperError;
        bool mStop = false;
    };
}

#endif
