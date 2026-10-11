#include "workqueue.hpp"

#include <components/debug/debuglog.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/debug/v3diagnostics.hpp>

#include <cstdint>
#include <algorithm>
#include <iterator>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>
#include <typeinfo>

namespace SceneUtil
{

    void WorkItem::waitTillDone()
    {
        std::optional<Debug::HitchTrace::Scope> trace;
        if (Debug::HitchTrace::requested())
        {
            trace.emplace("workqueue.wait", Debug::HitchTrace::CpuTiming::Scoped);
            trace->setMetric(0, "already_done", mDone.load());
            if (!mDone.load()) trace->retain();
        }
        if (mDone)
        {
            if (trace)
            {
                const std::lock_guard lock(mMutex);
                trace->setLink(mHitchCompletedEnqueue);
            }
            return;
        }

        std::unique_lock<std::mutex> lock(mMutex);
        while (!mDone)
        {
            mCondition.wait(lock);
        }
        if (trace) trace->setLink(mHitchCompletedEnqueue);
    }

    void WorkItem::signalDone(std::uint64_t hitchEnqueue)
    {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            // isDone is never reset. Repeated admissions execute normally,
            // but only the first false->true transition satisfies its waiters.
            if (Debug::HitchTrace::requested() && !mDone) mHitchCompletedEnqueue = hitchEnqueue;
            mDone = true;
        }
        mCondition.notify_all();
    }

    bool WorkItem::isDone() const
    {
        return mDone;
    }

    std::uint64_t WorkItem::diagnosticCompletionEnqueue()
    {
        if (!Debug::HitchTrace::requested()) return 0;
        const std::lock_guard lock(mMutex);
        return mHitchCompletedEnqueue;
    }

    WorkQueue::WorkQueue(std::size_t workerThreads)
        : mIsReleased(false)
    {
        start(workerThreads);
    }

    WorkQueue::~WorkQueue()
    {
        stop();
    }

    void WorkQueue::start(std::size_t workerThreads)
    {
        {
            const std::lock_guard lock(mMutex);
            mIsReleased = false;
        }
        while (mThreads.size() < workerThreads)
            mThreads.emplace_back(std::make_unique<WorkThread>(*this));
    }

    void WorkQueue::stop()
    {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            while (!mQueue.empty())
            {
                const auto& entry = mQueue.back();
                if (entry.mTrace.mEnqueue)
                {
                    Debug::HitchTrace::Scope cancellation("workqueue.cancel_before_start", {}, {}, entry.mTrace.mFrame,
                        Debug::HitchTrace::CpuTiming::WallEnvelope);
                    cancellation.retain();
                    cancellation.setLink(entry.mTrace.mEnqueue);
                    cancellation.setMetric(0, "queue_depth", mQueue.size());
                    cancellation.setMetric(1, "outcome", 2); // cancelled; original drop does not signalDone
                }
                mQueue.pop_back();
            }
            mIsReleased = true;
            mCondition.notify_all();
        }

        mThreads.clear();
    }

    void WorkQueue::addWorkItem(osg::ref_ptr<WorkItem> item, bool front, Priority priority)
    {
        if (item->isDone())
        {
            Log(Debug::Error) << "Error: trying to add a work item that is already completed";
            return;
        }

        auto& writer = Debug::V3Diagnostics::workQueueWriter();
        const bool profile = writer.enabled();
        const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
        const std::string typeName = profile ? typeid(*item).name() : std::string();
        std::size_t queueDepth = 0;
        WorkTraceMetadata metadata;
        std::optional<Debug::HitchTrace::Scope> enqueue;
        if (Debug::HitchTrace::requested())
        {
            metadata.mFrame = Debug::HitchTrace::current ? Debug::HitchTrace::current->frame()
                : Debug::V3HitchTelemetry::currentFrame();
            enqueue.emplace("workqueue.enqueue", typeid(*item).name(), std::string_view{}, metadata.mFrame,
                Debug::HitchTrace::CpuTiming::WallEnvelope);
            enqueue->retain();
            metadata.mEnqueue = enqueue->id();
        }
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (enqueue) metadata.mQueued = Debug::HitchTrace::Clock::now();
            if (front || priority == Priority::Urgent)
                mQueue.push_front({ std::move(item), metadata, Priority::Urgent });
            else if (priority == Priority::NearFuture && mQueue.size() <= 256)
            {
                const auto before = std::find_if(mQueue.begin(), mQueue.end(), [](const auto& entry) {
                    return entry.mPriority == Priority::Background;
                });
                mQueue.insert(before, { std::move(item), metadata, priority });
                ++PriorityStats::submitted;
            }
            else
                mQueue.push_back({ std::move(item), metadata });
            queueDepth = mQueue.size();
            if (enqueue)
            {
                enqueue->setMetric(0, "queue_depth", queueDepth);
                enqueue->setMetric(1, "front", front);
                enqueue->setMetric(2, "item_address", itemId);
            }
            mCondition.notify_one();
        }
        if (profile)
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::threadId() << ',' << Debug::V3Diagnostics::csvQuote("enqueue") << ',' << itemId << ','
                << Debug::V3Diagnostics::csvQuote(typeName) << ',' << queueDepth << ','
                << mV3ActiveThreads.load(std::memory_order_relaxed) << ",0";
            writer.writeLine(row.str());
        }
    }

    bool WorkQueue::promoteWorkItem(const WorkItem* item)
    {
        if (!item) return false;
        std::lock_guard lock(mMutex);
        // Bound gameplay inspection/reordering for an unexpected backlog.
        if (mIsReleased || mQueue.size() > 256) return false;
        const auto target = std::find_if(mQueue.begin(), mQueue.end(), [item](const auto& entry) {
            return entry.mItem.get() == item;
        });
        if (target == mQueue.end() || target->mPriority != Priority::Background) return false;
        const auto before = std::find_if(mQueue.begin(), target, [](const auto& entry) {
            return entry.mPriority == Priority::Background;
        });
        target->mPriority = Priority::NearFuture;
        std::rotate(before, target, std::next(target));
        ++PriorityStats::promoted;
        return true;
    }

    osg::ref_ptr<WorkItem> WorkQueue::removeWorkItem(WorkTraceMetadata* trace)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        while (mQueue.empty() && !mIsReleased)
        {
            mCondition.wait(lock);
        }
        if (!mQueue.empty())
        {
            auto selected = mQueue.begin();
            if (selected->mPriority == Priority::NearFuture && mNearFutureBurst >= MaximumNearFutureBurst)
            {
                const auto background = std::find_if(mQueue.begin(), mQueue.end(), [](const auto& entry) {
                    return entry.mPriority == Priority::Background;
                });
                if (background != mQueue.end()) { selected = background; ++PriorityStats::fairness; }
            }
            QueueEntry entry = std::move(*selected);
            mQueue.erase(selected);
            if (entry.mPriority == Priority::NearFuture)
            {
                if (mNearFutureBurst < MaximumNearFutureBurst) ++mNearFutureBurst;
                ++PriorityStats::consumed;
            }
            else if (entry.mPriority == Priority::Background) mNearFutureBurst = 0;
            if (trace)
            {
                *trace = entry.mTrace;
                if (trace->mEnqueue)
                    trace->mDepth = mQueue.size();
            }
            return std::move(entry.mItem);
        }
        return nullptr;
    }

    size_t WorkQueue::getNumItems() const
    {
        std::unique_lock<std::mutex> lock(mMutex);
        return mQueue.size();
    }

    size_t WorkQueue::getNumActiveThreads() const
    {
        return std::accumulate(
            mThreads.begin(), mThreads.end(), 0u, [](auto r, const auto& t) { return r + t->isActive(); });
    }

    WorkThread::WorkThread(WorkQueue& workQueue)
        : mWorkQueue(&workQueue)
        , mActive(false)
        , mThread([this] { run(); })
    {
    }

    WorkThread::~WorkThread()
    {
        mThread.join();
    }

    void WorkThread::run()
    {
        while (true)
        {
            WorkTraceMetadata metadata;
            osg::ref_ptr<WorkItem> item = mWorkQueue->removeWorkItem(&metadata);
            if (!item)
                return;
            mActive = true;

            auto& writer = Debug::V3Diagnostics::workQueueWriter();
            const bool profile = writer.enabled();
            const bool traceProfile = Debug::V3Diagnostics::traceWriter().enabled();
            if (profile)
                mWorkQueue->mV3ActiveThreads.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
            const std::string typeName = (profile || traceProfile || metadata.mEnqueue) ? typeid(*item).name() : std::string();
            const auto start = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            Debug::V3Diagnostics::TraceScope trace("workqueue", typeName, std::to_string(itemId), 0.05);

            if (profile)
            {
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                    << Debug::V3Diagnostics::threadId() << ',' << Debug::V3Diagnostics::csvQuote("start") << ',' << itemId << ','
                    << Debug::V3Diagnostics::csvQuote(typeName) << ',' << mWorkQueue->getNumItems() << ','
                    << mWorkQueue->mV3ActiveThreads.load(std::memory_order_relaxed) << ",0";
                writer.writeLine(row.str());
            }

            std::optional<Debug::HitchTrace::Scope> work;
            if (metadata.mEnqueue)
            {
                work.emplace("workqueue.execute", typeName, std::string_view{}, metadata.mFrame,
                    Debug::HitchTrace::CpuTiming::Scoped);
                work->setLink(metadata.mEnqueue);
                Debug::HitchTrace::Scope started("workqueue.start", {}, {}, metadata.mFrame,
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                started.retain(); started.setLink(metadata.mEnqueue);
                started.setMetric(0, "queue_delay_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Debug::HitchTrace::Clock::now() - metadata.mQueued).count());
                started.setMetric(1, "queue_depth", metadata.mDepth);
                started.setMetric(2, "item_address", itemId);
            }
            try
            {
                item->doWork();
                item->signalDone(metadata.mEnqueue);
            }
            catch (...)
            {
                if (work)
                {
                    Debug::HitchTrace::Scope outcome("workqueue.complete", {}, {}, metadata.mFrame,
                        Debug::HitchTrace::CpuTiming::WallEnvelope);
                    outcome.retain(); outcome.setLink(metadata.mEnqueue);
                    outcome.setMetric(0, "outcome", 1); // exception; preserve original propagation
                }
                throw;
            }
            if (work)
            {
                Debug::HitchTrace::Scope outcome("workqueue.complete", {}, {}, metadata.mFrame,
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                outcome.retain(); outcome.setLink(metadata.mEnqueue);
                outcome.setMetric(0, "outcome", 0);
            }

            if (profile)
            {
                const double durationMs = Debug::V3Diagnostics::elapsedMs(start);
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                    << Debug::V3Diagnostics::threadId() << ',' << Debug::V3Diagnostics::csvQuote("end") << ',' << itemId << ','
                    << Debug::V3Diagnostics::csvQuote(typeName) << ',' << mWorkQueue->getNumItems() << ','
                    << mWorkQueue->mV3ActiveThreads.load(std::memory_order_relaxed) << ',' << std::fixed
                    << std::setprecision(3) << durationMs;
                writer.writeLine(row.str());
            }
            if (profile)
                mWorkQueue->mV3ActiveThreads.fetch_sub(1, std::memory_order_relaxed);
            mActive = false;
        }
    }

    bool WorkThread::isActive() const
    {
        return mActive;
    }

}
