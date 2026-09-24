#ifndef OPENMW_COMPONENTS_RENDERCORE_BOUNDEDPARALLELFOR_H
#define OPENMW_COMPONENTS_RENDERCORE_BOUNDEDPARALLELFOR_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace RenderCore
{
    // Bounded, persistent CPU-only workers. Submission is synchronous: no job
    // or borrowed input survives forEach(). The caller participates. Not
    // reentrant; callbacks must not submit work to this same pool.
    class BoundedParallelFor final
    {
    public:
        explicit BoundedParallelFor(std::size_t workers, std::size_t minimumCount = 128, std::size_t chunkSize = 8)
            : mMinimumCount(std::max(std::size_t{1}, minimumCount)), mChunkSize(std::max(std::size_t{1}, chunkSize))
        {
            workers = std::min(workers, std::size_t{3});
            mWorkers.reserve(workers);
            try
            {
                for (std::size_t i = 0; i < workers; ++i)
                    mWorkers.emplace_back([this](std::stop_token stop) { run(stop); });
            }
            catch (const std::system_error&)
            {
                // Thread-resource exhaustion must not make the opt-in path
                // unusable. Retain the synchronous inline implementation.
                for (auto& worker : mWorkers) worker.request_stop();
                mWorkers.clear();
            }
        }

        ~BoundedParallelFor()
        {
            // Request all stops before joining any worker. The stop-aware
            // condition wait also handles partial-construction destruction.
            for (auto& worker : mWorkers)
                worker.request_stop();
            mReady.notify_all();
            mWorkers.clear();
        }

        BoundedParallelFor(const BoundedParallelFor&) = delete;
        BoundedParallelFor& operator=(const BoundedParallelFor&) = delete;
        [[nodiscard]] std::size_t workers() const noexcept { return mWorkers.size(); }
        [[nodiscard]] std::size_t workersFor(std::size_t count) const noexcept
        { return count >= mMinimumCount ? workers() : 0; }

        void forEach(std::size_t count, const std::function<void(std::size_t)>& function)
        {
            if (workersFor(count) == 0)
            {
                for (std::size_t i = 0; i < count; ++i)
                    function(i);
                return;
            }
            // Serialize callers, including the lifetime of the borrowed function.
            const std::lock_guard submit(mSubmission);
            {
                const std::lock_guard lock(mMutex);
                mFunction = &function;
                mCount = count;
                mNext.store(0, std::memory_order_relaxed);
                mFailed.store(false, std::memory_order_relaxed);
                mException = nullptr;
                mPending = mWorkers.size();
                ++mGeneration;
            }
            mReady.notify_all();
            execute();
            std::unique_lock lock(mMutex);
            mFinished.wait(lock, [this] { return mPending == 0; });
            mFunction = nullptr;
            if (mException)
                std::rethrow_exception(mException);
        }

    private:
        void execute() noexcept
        {
            try
            {
                while (!mFailed.load(std::memory_order_relaxed))
                {
                    // Small chunks balance differently sized geometry payloads
                    // without an atomic increment for every individual draw.
                    const auto begin = mNext.fetch_add(mChunkSize, std::memory_order_relaxed);
                    if (begin >= mCount)
                        return;
                    const auto end = begin + std::min(mChunkSize, mCount - begin);
                    for (auto i = begin; i < end; ++i)
                        (*mFunction)(i);
                }
            }
            catch (...)
            {
                const std::lock_guard lock(mMutex);
                if (!mException)
                    mException = std::current_exception();
                mFailed.store(true, std::memory_order_relaxed);
            }
        }

        void run(std::stop_token stop)
        {
            std::uint64_t generation = 0;
            for (;;)
            {
                std::unique_lock lock(mMutex);
                mReady.wait(lock, stop, [&] { return generation != mGeneration; });
                if (stop.stop_requested())
                    return;
                generation = mGeneration;
                lock.unlock();
                execute();
                lock.lock();
                if (--mPending == 0)
                    mFinished.notify_one();
            }
        }

        const std::size_t mMinimumCount;
        const std::size_t mChunkSize;
        std::mutex mSubmission;
        std::mutex mMutex;
        std::condition_variable_any mReady;
        std::condition_variable mFinished;
        const std::function<void(std::size_t)>* mFunction = nullptr;
        std::size_t mCount = 0;
        std::size_t mPending = 0;
        std::uint64_t mGeneration = 0;
        std::atomic_size_t mNext{0};
        std::atomic_bool mFailed{false};
        std::exception_ptr mException;
        // Declared last: partially constructed pools join workers before the
        // synchronization objects disappear.
        std::vector<std::jthread> mWorkers;
    };
}
#endif
