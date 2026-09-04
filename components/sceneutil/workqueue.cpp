#include "workqueue.hpp"

#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>

#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <typeinfo>

namespace SceneUtil
{

    void WorkItem::waitTillDone()
    {
        if (mDone)
            return;

        std::unique_lock<std::mutex> lock(mMutex);
        while (!mDone)
        {
            mCondition.wait(lock);
        }
    }

    void WorkItem::signalDone()
    {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mDone = true;
        }
        mCondition.notify_all();
    }

    bool WorkItem::isDone() const
    {
        return mDone;
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
                mQueue.pop_back();
            mIsReleased = true;
            mCondition.notify_all();
        }

        mThreads.clear();
    }

    void WorkQueue::addWorkItem(osg::ref_ptr<WorkItem> item, bool front)
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
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (front)
                mQueue.push_front(std::move(item));
            else
                mQueue.push_back(std::move(item));
            queueDepth = mQueue.size();
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

    osg::ref_ptr<WorkItem> WorkQueue::removeWorkItem()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        while (mQueue.empty() && !mIsReleased)
        {
            mCondition.wait(lock);
        }
        if (!mQueue.empty())
        {
            osg::ref_ptr<WorkItem> item = std::move(mQueue.front());
            mQueue.pop_front();
            return item;
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
            osg::ref_ptr<WorkItem> item = mWorkQueue->removeWorkItem();
            if (!item)
                return;
            mActive = true;

            auto& writer = Debug::V3Diagnostics::workQueueWriter();
            const bool profile = writer.enabled();
            const bool traceProfile = Debug::V3Diagnostics::traceWriter().enabled();
            if (profile)
                mWorkQueue->mV3ActiveThreads.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
            const std::string typeName = (profile || traceProfile) ? typeid(*item).name() : std::string();
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

            item->doWork();
            item->signalDone();

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
