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

namespace SceneUtil
{
    // One persistent helper for coarse thread-safe work. There is no FIFO:
    // concurrent users fail open to their serial path. The caller owns half
    // of the range while the helper owns the other half. Publication remains
    // the caller's responsibility after run() returns.
    class BoundedTwoWayWork final
    {
    public:
        using RangeTask = std::function<void(std::size_t begin, std::size_t end, bool helper)>;

        static bool run(std::size_t count, std::size_t minimumCount, RangeTask task)
        {
            return instance().runImpl(count, minimumCount, std::move(task));
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
            if (mWorker.joinable())
                mWorker.join();
        }

        bool ensureWorker()
        {
            if (mWorker.joinable())
                return true;
            try
            {
                mWorker = std::thread([this] { workerLoop(); });
                return true;
            }
            catch (const std::system_error&)
            {
                return false;
            }
        }

        bool runImpl(std::size_t count, std::size_t minimumCount, RangeTask task)
        {
            if (!task || count < (std::max)(std::size_t{ 2 }, minimumCount))
                return false;

            std::unique_lock gate(mGateMutex, std::try_to_lock);
            if (!gate.owns_lock() || !ensureWorker())
                return false;

            const std::size_t middle = count / 2;
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(mStateMutex);
                mTask = task;
                mBegin = middle;
                mEnd = count;
                mHelperError = nullptr;
                generation = ++mGeneration;
            }
            mWork.notify_one();

            std::exception_ptr callerError;
            try
            {
                task(0, middle, false);
            }
            catch (...)
            {
                callerError = std::current_exception();
            }

            std::exception_ptr helperError;
            {
                std::unique_lock lock(mStateMutex);
                mDone.wait(lock, [this, generation] {
                    return mStop || mCompletedGeneration >= generation;
                });
                helperError = mHelperError;
            }

            if (callerError)
                std::rethrow_exception(callerError);
            if (helperError)
                std::rethrow_exception(helperError);
            return !mStop;
        }

        void workerLoop()
        {
            std::uint64_t seenGeneration = 0;
            for (;;)
            {
                RangeTask task;
                std::size_t begin = 0;
                std::size_t end = 0;
                std::uint64_t generation = 0;
                {
                    std::unique_lock lock(mStateMutex);
                    mWork.wait(lock, [this, seenGeneration] {
                        return mStop || mGeneration != seenGeneration;
                    });
                    if (mStop)
                        return;

                    generation = mGeneration;
                    seenGeneration = generation;
                    task = mTask;
                    begin = mBegin;
                    end = mEnd;
                }

                std::exception_ptr error;
                try
                {
                    task(begin, end, true);
                }
                catch (...)
                {
                    error = std::current_exception();
                }

                {
                    std::lock_guard lock(mStateMutex);
                    if (generation == mGeneration)
                    {
                        mHelperError = error;
                        mCompletedGeneration = generation;
                        mTask = {};
                    }
                }
                mDone.notify_one();
            }
        }

        std::mutex mGateMutex;
        std::mutex mStateMutex;
        std::condition_variable mWork;
        std::condition_variable mDone;
        std::thread mWorker;
        RangeTask mTask;
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;
        std::uint64_t mGeneration = 0;
        std::uint64_t mCompletedGeneration = 0;
        std::exception_ptr mHelperError;
        bool mStop = false;
    };
}

#endif
