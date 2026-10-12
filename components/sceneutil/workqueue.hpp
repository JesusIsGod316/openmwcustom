#ifndef OPENMW_COMPONENTS_SCENEUTIL_WORKQUEUE_H
#define OPENMW_COMPONENTS_SCENEUTIL_WORKQUEUE_H

#include <osg/Referenced>
#include <osg/ref_ptr>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace SceneUtil
{
    // One admission, rather than one object address: the same WorkItem can be
    // submitted more than once before it completes. Queue ownership publishes
    // this immutable snapshot to the worker without a second synchronization.
    struct WorkTraceMetadata
    {
        std::uint64_t mEnqueue = 0;
        unsigned mFrame = 0;
        std::chrono::steady_clock::time_point mQueued{};
        std::size_t mDepth = 0;
    };

    class WorkItem : public osg::Referenced
    {
    public:
        /// Override in a derived WorkItem to perform actual work.
        virtual void doWork() {}

        bool isDone() const;

        /// Wait until the work is completed. Usually called from the main thread.
        void waitTillDone();

        /// Internal use by the WorkQueue.
        void signalDone(std::uint64_t hitchEnqueue = 0);

        /// Diagnostic admission that first satisfied waitTillDone. Read-only;
        /// unused by gameplay and zero when Deep tracing was not requested.
        std::uint64_t diagnosticCompletionEnqueue();

        /// Set abort flag in order to return from doWork() as soon as possible. May not be respected by all WorkItems.
        virtual void abort() {}

    private:
        std::atomic_bool mDone{ false };
        std::mutex mMutex;
        std::condition_variable mCondition;
        std::uint64_t mHitchCompletedEnqueue = 0; // guarded by mMutex; actual wait satisfier
    };

    class WorkThread;

    /// @brief A work queue that users can push work items onto, to be completed by one or more background threads.
    /// @note Work items will be processed in the order that they were given in, however
    /// if multiple work threads are involved then it is possible for a later item to complete before earlier items.
    class WorkQueue : public osg::Referenced
    {
    public:
        enum class Priority : unsigned char { Background, NearFuture, Urgent };
        static constexpr unsigned MaximumNearFutureBurst = 4;
        struct PriorityStats
        {
            inline static std::atomic_uint64_t submitted{0}, promoted{0}, consumed{0}, fairness{0};
        };
        WorkQueue(std::size_t workerThreads);
        ~WorkQueue();

        void start(std::size_t workerThreads);

        void stop();

        /// Add a new work item to the back of the queue.
        /// @par The work item's waitTillDone() method may be used by the caller to wait until the work is complete.
        /// @param front If true, add item to the front of the queue. If false (default), add to the back.
        void addWorkItem(osg::ref_ptr<WorkItem> item, bool front = false,
            Priority priority = Priority::Background);

        // Reorder the same still-queued admission. No new owner/reservation,
        // waiting, or preemption of a running producer.
        bool promoteWorkItem(const WorkItem* item);

        /// Get the next work item from the front of the queue. If the queue is empty, waits until a new item is added.
        /// If the workqueue is in the process of being destroyed, may return nullptr.
        /// @par Used internally by the WorkThread.
        osg::ref_ptr<WorkItem> removeWorkItem(WorkTraceMetadata* trace = nullptr);

        size_t getNumItems() const;

        size_t getNumActiveThreads() const;

    private:
        bool mIsReleased;
        struct QueueEntry
        {
            osg::ref_ptr<WorkItem> mItem;
            WorkTraceMetadata mTrace;
            Priority mPriority = Priority::Background;
        };
        std::deque<QueueEntry> mQueue;
        unsigned mNearFutureBurst = 0;

        mutable std::mutex mMutex;
        std::condition_variable mCondition;

        std::vector<std::unique_ptr<WorkThread>> mThreads;
        std::atomic_size_t mV3ActiveThreads{ 0 };

        friend class WorkThread;
    };

    /// Internally used by WorkQueue.
    class WorkThread
    {
    public:
        WorkThread(WorkQueue& workQueue);

        ~WorkThread();

        bool isActive() const;

    private:
        WorkQueue* mWorkQueue;
        std::atomic<bool> mActive;
        std::thread mThread;

        void run();
    };

}

#endif
