#ifndef OPENMW_COMPONENTS_SCENEUTIL_FRAMEJOBSERVICE_H
#define OPENMW_COMPONENTS_SCENEUTIL_FRAMEJOBSERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <components/debug/v3deeptelemetry.hpp>
#include <utility>

namespace SceneUtil
{
    // V3.24: a deliberately queue-less frame-job service for latency-sensitive work.
    //
    // Critical and opportunistic work use separate reserved lanes. A submission is
    // accepted only when the selected lane is idle *now*. Nothing is placed behind
    // background/preload work, so callers can either run critical work inline or
    // skip opportunistic work without creating a queue-then-wait priority inversion.
    class FrameJobService
    {
    public:
        enum class Lane
        {
            Critical,
            Opportunistic,
        };

        struct Stats
        {
            std::uint64_t submitted = 0;
            std::uint64_t rejectedBusy = 0;
            std::uint64_t callerRuns = 0;
            std::uint64_t skipped = 0;
        };

        static FrameJobService& instance()
        {
            static FrameJobService service;
            return service;
        }

        FrameJobService(const FrameJobService&) = delete;
        FrameJobService& operator=(const FrameJobService&) = delete;

        bool trySubmit(Lane lane, std::uint64_t generation, std::function<void()> task)
        {
            if (!Debug::V324DeepTelemetry::enabled())
                return worker(lane).trySubmit(generation, std::move(task));

            const std::string laneName = lane == Lane::Critical ? "critical" : "opportunistic";
            auto wrapped = [laneName, generation, task = std::move(task)]() mutable {
                Debug::V324DeepTelemetry::Scope scope("framejob", "execute",
                    laneName + ":" + std::to_string(generation));
                task();
            };
            const bool accepted = worker(lane).trySubmit(generation, std::move(wrapped));
            Debug::V324DeepTelemetry::event("framejob", accepted ? "submit_accepted" : "submit_rejected",
                laneName + ":" + std::to_string(generation));
            return accepted;
        }

        bool isIdle(Lane lane) const { return worker(lane).isIdle(); }

        bool isComplete(Lane lane, std::uint64_t generation) const
        {
            return worker(lane).completedGeneration() >= generation;
        }

        bool failed(Lane lane, std::uint64_t generation) const
        {
            return worker(lane).failedGeneration() == generation;
        }

        // Waiting is permitted only for work that was admitted immediately to a
        // reserved lane. This service never queues a task and then waits for it to
        // reach a worker. Opportunistic frame paths should normally use isComplete
        // and fail open instead of calling wait.
        void wait(Lane lane, std::uint64_t generation)
        {
            Debug::V324DeepTelemetry::Scope scope("framejob", "wait");
            worker(lane).wait(generation);
        }

        void noteCallerRuns(Lane lane)
        {
            worker(lane).noteCallerRuns();
            Debug::V324DeepTelemetry::event("framejob", "caller_runs",
                lane == Lane::Critical ? "critical" : "opportunistic");
        }
        void noteSkipped(Lane lane)
        {
            worker(lane).noteSkipped();
            Debug::V324DeepTelemetry::event("framejob", "skipped",
                lane == Lane::Critical ? "critical" : "opportunistic");
        }
        Stats stats(Lane lane) const { return worker(lane).stats(); }

    private:
        class ImmediateWorker
        {
        public:
            ImmediateWorker()
                : mThread([this] { run(); })
            {
            }

            ~ImmediateWorker()
            {
                {
                    std::lock_guard lock(mMutex);
                    mStop = true;
                }
                mWork.notify_one();
                if (mThread.joinable())
                    mThread.join();
            }

            bool trySubmit(std::uint64_t generation, std::function<void()> task)
            {
                std::lock_guard lock(mMutex);
                if (mStop || mBusy.load(std::memory_order_relaxed) || generation <= mLastSubmitted)
                {
                    mRejectedBusy.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }

                mTask = std::move(task);
                mGeneration = generation;
                mLastSubmitted = generation;
                mBusy.store(true, std::memory_order_release);
                mSubmitted.fetch_add(1, std::memory_order_relaxed);
                mWork.notify_one();
                return true;
            }

            bool isIdle() const { return !mBusy.load(std::memory_order_acquire); }

            std::uint64_t completedGeneration() const
            {
                return mCompletedGeneration.load(std::memory_order_acquire);
            }

            std::uint64_t failedGeneration() const
            {
                return mFailedGeneration.load(std::memory_order_acquire);
            }

            void wait(std::uint64_t generation)
            {
                if (completedGeneration() >= generation)
                    return;
                std::unique_lock lock(mMutex);
                mDone.wait(lock, [this, generation] {
                    return mStop || mCompletedGeneration.load(std::memory_order_acquire) >= generation;
                });
            }

            void noteCallerRuns() { mCallerRuns.fetch_add(1, std::memory_order_relaxed); }
            void noteSkipped() { mSkipped.fetch_add(1, std::memory_order_relaxed); }

            Stats stats() const
            {
                return Stats{
                    mSubmitted.load(std::memory_order_relaxed),
                    mRejectedBusy.load(std::memory_order_relaxed),
                    mCallerRuns.load(std::memory_order_relaxed),
                    mSkipped.load(std::memory_order_relaxed),
                };
            }

        private:
            void run()
            {
                while (true)
                {
                    std::function<void()> task;
                    std::uint64_t generation = 0;
                    {
                        std::unique_lock lock(mMutex);
                        mWork.wait(lock, [this] { return mStop || mBusy.load(std::memory_order_acquire); });
                        if (mStop)
                            return;
                        task = std::move(mTask);
                        generation = mGeneration;
                    }

                    try
                    {
                        task();
                    }
                    catch (...)
                    {
                        mFailedGeneration.store(generation, std::memory_order_release);
                    }

                    {
                        std::lock_guard lock(mMutex);
                        mTask = {};
                        mCompletedGeneration.store(generation, std::memory_order_release);
                        mBusy.store(false, std::memory_order_release);
                    }
                    mDone.notify_all();
                }
            }

            mutable std::mutex mMutex;
            std::condition_variable mWork;
            std::condition_variable mDone;
            std::function<void()> mTask;
            std::uint64_t mGeneration = 0;
            std::uint64_t mLastSubmitted = 0;
            std::atomic<std::uint64_t> mCompletedGeneration{ 0 };
            std::atomic<std::uint64_t> mFailedGeneration{ 0 };
            std::atomic<std::uint64_t> mSubmitted{ 0 };
            std::atomic<std::uint64_t> mRejectedBusy{ 0 };
            std::atomic<std::uint64_t> mCallerRuns{ 0 };
            std::atomic<std::uint64_t> mSkipped{ 0 };
            std::atomic_bool mBusy{ false };
            bool mStop = false;
            std::thread mThread;
        };

        FrameJobService() = default;
        ~FrameJobService() = default;

        ImmediateWorker& worker(Lane lane)
        {
            return lane == Lane::Critical ? mCritical : mOpportunistic;
        }

        const ImmediateWorker& worker(Lane lane) const
        {
            return lane == Lane::Critical ? mCritical : mOpportunistic;
        }

        ImmediateWorker mCritical;
        ImmediateWorker mOpportunistic;
    };
}

#endif
