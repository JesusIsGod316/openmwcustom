#include "warmqueue.hpp"
#include <algorithm>
#include <utility>

namespace MWSound
{
    WarmQueue::WarmQueue(Executor execute, std::function<void()> threadInit,
        std::size_t maxItems, std::size_t maxPathBytes)
        : mExecute(std::move(execute)), mThreadInit(std::move(threadInit)),
          mMaxItems(maxItems), mMaxPathBytes(maxPathBytes), mThread([this]{run();}) {}

    WarmQueue::~WarmQueue()
    {
        {
            std::lock_guard lock(mMutex);
            mStop = true;
        }
        mCondition.notify_one();
        mThread.join(); // VFS/cache owners must remain alive until here
    }

    WarmQueue::Result WarmQueue::enqueue(Item item)
    {
        if (item.path.empty() || item.path.size()>4096) return Result::Invalid;
        std::unique_lock lock(mMutex);
        if (mStop) return Result::Stopping;
        if (mRunning && mRunning->path==item.path)
        {
            mUpgradeRunning |= item.wholeFile && !mRunning->wholeFile;
            ++mStats.deduplicated;
            return Result::Duplicate;
        }
        if (auto it=mQueued.find(item.path); it!=mQueued.end())
        {
            it->second->wholeFile |= item.wholeFile;
            if (item.urgent && !it->second->urgent)
            {
                it->second->urgent=true;
                mQueue.splice(mQueue.begin(),mQueue,it->second);
            }
            ++mStats.deduplicated;
            return Result::Duplicate;
        }
        if (mStats.pending>=mMaxItems || item.path.size()>mMaxPathBytes-mStats.pathBytes)
        { ++mStats.dropped; return Result::Full; }
        const auto it = mQueue.insert(item.urgent ? mQueue.begin() : mQueue.end(),std::move(item));
        try { mQueued.emplace(it->path,it); }
        catch (...) { mQueue.erase(it); throw; }
        ++mStats.pending;
        mStats.pathBytes += it->path.size();
        ++mStats.accepted;
        mStats.peakPending=std::max(mStats.peakPending,mStats.pending);
        lock.unlock();
        mCondition.notify_one();
        return Result::Accepted;
    }

    WarmQueue::Stats WarmQueue::stats() const { std::lock_guard lock(mMutex); return mStats; }

    void WarmQueue::run() noexcept
    {
        try { if (mThreadInit) mThreadInit(); }
        catch (...) { std::lock_guard lock(mMutex); ++mStats.failed; }
        for (;;)
        {
            std::unique_lock lock(mMutex);
            mCondition.wait(lock,[&]{return mStop || !mQueue.empty();});
            if (mStop) return;
            mRunning.emplace(std::move(mQueue.front()));
            mQueue.pop_front();
            mQueued.erase(mRunning->path);
            mUpgradeRunning=false;
            // Producers never modify the in-flight Item; only its upgrade flag.
            // Its owned path remains stable throughout the unlocked callback.
            lock.unlock();
            bool ok=false, failed=false;
            try { ok=mExecute && mExecute(*mRunning); }
            catch (...) { failed=true; }
            lock.lock();
            ++mStats.processed;
            if (failed) ++mStats.failed;
            else if (!ok) ++mStats.skipped;
            if (mUpgradeRunning && !mStop)
            {
                // Upgrade on the same worker without allocating another queue
                // entry or violating the outstanding-name budget.
                mRunning->wholeFile=true;
                mUpgradeRunning=false;
                lock.unlock();
                ok=false;failed=false;
                try { ok=mExecute && mExecute(*mRunning); }
                catch (...) { failed=true; }
                lock.lock();
                ++mStats.processed;
                if(failed) ++mStats.failed;
                else if(!ok) ++mStats.skipped;
            }
            --mStats.pending;
            mStats.pathBytes-=mRunning->path.size();
            mRunning.reset();
        }
    }
}
