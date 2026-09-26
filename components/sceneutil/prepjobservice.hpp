#ifndef OPENMW_COMPONENTS_SCENEUTIL_PREPJOBSERVICE_H
#define OPENMW_COMPONENTS_SCENEUTIL_PREPJOBSERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace SceneUtil
{
    // P7 queue-less two-way CPU preparation. Critical demand work and
    // background preload work have separate helpers, so speculative work
    // cannot occupy the helper needed by a cull-time miss.
    class PrepJobService final
    {
    public:
        enum class Lane { Critical, Background };
        using Task = std::function<void()>;

        struct LaneStats
        {
            std::uint64_t admittedPairs = 0;
            std::uint64_t busyFallbacks = 0;
            std::uint64_t completedPairs = 0;
            std::uint64_t failedPairs = 0;
        };

        static PrepJobService& instance()
        {
            static PrepJobService service;
            return service;
        }

        PrepJobService(const PrepJobService&) = delete;
        PrepJobService& operator=(const PrepJobService&) = delete;

        // Returns false only if the lane is already occupied. In that case
        // neither task runs and the caller preserves the exact serial fallback.
        bool runPair(Lane lane, Task helperTask, Task callerTask)
        {
            ImmediateWorker& w = worker(lane);
            const auto generation = w.tryStart(std::move(helperTask));
            if (!generation)
            {
                stats(lane).busyFallbacks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            stats(lane).admittedPairs.fetch_add(1, std::memory_order_relaxed);

            std::exception_ptr callerError;
            try { callerTask(); }
            catch (...) { callerError = std::current_exception(); }

            const std::exception_ptr helperError = w.wait(*generation);
            if (callerError || helperError)
            {
                stats(lane).failedPairs.fetch_add(1, std::memory_order_relaxed);
                if (callerError) std::rethrow_exception(callerError);
                std::rethrow_exception(helperError);
            }

            stats(lane).completedPairs.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        LaneStats snapshot(Lane lane) const
        {
            const AtomicLaneStats& value = stats(lane);
            return {
                value.admittedPairs.load(std::memory_order_relaxed),
                value.busyFallbacks.load(std::memory_order_relaxed),
                value.completedPairs.load(std::memory_order_relaxed),
                value.failedPairs.load(std::memory_order_relaxed),
            };
        }

    private:
        struct AtomicLaneStats
        {
            std::atomic<std::uint64_t> admittedPairs{0};
            std::atomic<std::uint64_t> busyFallbacks{0};
            std::atomic<std::uint64_t> completedPairs{0};
            std::atomic<std::uint64_t> failedPairs{0};
        };

        class ImmediateWorker
        {
        public:
            ImmediateWorker() : mThread([this] { run(); }) {}
            ~ImmediateWorker()
            {
                {
                    std::lock_guard lock(mMutex);
                    mStop = true;
                }
                mWork.notify_all();
                mDone.notify_all();
                if (mThread.joinable()) mThread.join();
            }

            std::optional<std::uint64_t> tryStart(Task task)
            {
                std::lock_guard lock(mMutex);
                if (mStop || mBusy || mAwaitingAck) return std::nullopt;
                mTask = std::move(task);
                const std::uint64_t generation = ++mGeneration;
                mBusy = true;
                mWork.notify_one();
                return generation;
            }

            std::exception_ptr wait(std::uint64_t generation)
            {
                std::unique_lock lock(mMutex);
                mDone.wait(lock, [this, generation] {
                    return mStop || (mCompletedGeneration >= generation && mAwaitingAck);
                });
                if (mStop && mCompletedGeneration < generation)
                    return std::make_exception_ptr(std::runtime_error("P7 prep worker stopped"));
                std::exception_ptr error = mError;
                mError = nullptr;
                mAwaitingAck = false;
                return error;
            }

        private:
            void run()
            {
                for (;;)
                {
                    Task task;
                    std::uint64_t generation = 0;
                    {
                        std::unique_lock lock(mMutex);
                        mWork.wait(lock, [this] { return mStop || mBusy; });
                        if (mStop) return;
                        task = std::move(mTask);
                        generation = mGeneration;
                    }

                    std::exception_ptr error;
                    try { task(); }
                    catch (...) { error = std::current_exception(); }

                    {
                        std::lock_guard lock(mMutex);
                        mTask = {};
                        mError = error;
                        mCompletedGeneration = generation;
                        mBusy = false;
                        mAwaitingAck = true;
                    }
                    mDone.notify_all();
                }
            }

            std::mutex mMutex;
            std::condition_variable mWork;
            std::condition_variable mDone;
            Task mTask;
            std::thread mThread;
            std::exception_ptr mError;
            std::uint64_t mGeneration = 0;
            std::uint64_t mCompletedGeneration = 0;
            bool mBusy = false;
            bool mAwaitingAck = false;
            bool mStop = false;
        };

        PrepJobService() = default;
        ~PrepJobService()
        {
            const char* path = std::getenv("OPENMW_P7_PREP_STATS_FILE");
            if (!path || path[0] == '\0') return;
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out) return;
            out << "lane,admitted_pairs,busy_fallbacks,completed_pairs,failed_pairs\n";
            writeStats(out, "critical", snapshot(Lane::Critical));
            writeStats(out, "background", snapshot(Lane::Background));
        }

        static void writeStats(std::ofstream& out, const char* name, const LaneStats& value)
        {
            out << name << ',' << value.admittedPairs << ',' << value.busyFallbacks << ','
                << value.completedPairs << ',' << value.failedPairs << '\n';
        }

        ImmediateWorker& worker(Lane lane) { return lane == Lane::Critical ? mCritical : mBackground; }
        const AtomicLaneStats& stats(Lane lane) const
        {
            return lane == Lane::Critical ? mCriticalStats : mBackgroundStats;
        }
        AtomicLaneStats& stats(Lane lane)
        {
            return lane == Lane::Critical ? mCriticalStats : mBackgroundStats;
        }

        ImmediateWorker mCritical;
        ImmediateWorker mBackground;
        AtomicLaneStats mCriticalStats;
        AtomicLaneStats mBackgroundStats;
    };
}
#endif
