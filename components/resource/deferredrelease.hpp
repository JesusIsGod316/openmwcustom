#ifndef OPENMW_COMPONENTS_RESOURCE_DEFERREDRELEASE_H
#define OPENMW_COMPONENTS_RESOURCE_DEFERREDRELEASE_H
#include "cachemaintenance.hpp"
#include <osg/Referenced>
#include <osg/ref_ptr>
#include <deque>
#include <mutex>
#include <cstdint>

namespace Resource
{
    class DeferredReleaseQueue
    {
    public:
        struct Stats { std::size_t owners = 0; std::uint64_t estimatedBytes = 0; };
        DeferredReleaseQueue(std::size_t maxOwners = 16, std::uint64_t maxBytes = 512ull*1024*1024)
            : mMaximumOwners(maxOwners), mMaximumBytes(maxBytes) {}
        // Never enqueue a running WorkItem. The caller must prove completion;
        // queue deliberately knows nothing about worker/job types or GL calls.
        template<class T>
        bool push(osg::ref_ptr<T>& object, std::uint64_t estimate)
        {
            if (!object) return false;
            Owner incoming{object.get(), estimate};
            std::lock_guard lock(mMutex);
            if (mOwners.size() + mReleasing >= mMaximumOwners || estimate > UINT64_MAX - mBytes
                || (mBytes && (mBytes >= mMaximumBytes || estimate > mMaximumBytes - mBytes))) return false;
            mOwners.push_back(std::move(incoming)); mBytes += estimate;
            // OSG 3.6 ref_ptr may copy even from an rvalue. Clear the
            // temporary while the queue is still protected, so a fast
            // consumer cannot leave it as the final submitting-thread owner.
            incoming.object = nullptr;
            // Publish only after removing the caller's retaining reference.
            // Otherwise a consumer could release first, leaving the caller to
            // run the last destructor on the main thread after this returns.
            object = nullptr; // queue now owns a ref; this cannot be final
            return true;
        }
        void drain(CacheMaintenanceBudget& budget)
        {
            while (budget.available())
            {
                Owner owner;
                {
                    std::lock_guard lock(mMutex);
                    if (mOwners.empty() || !budget.release(mOwners.front().estimate)) break;
                    owner = std::move(mOwners.front()); mOwners.pop_front(); ++mReleasing;
                }
                owner.object = nullptr; // may reenter this queue/cache
                {
                    std::lock_guard lock(mMutex);
                    mBytes -= owner.estimate; --mReleasing;
                }
            }
        }
        Stats stats() const
        { std::lock_guard lock(mMutex); return {mOwners.size() + mReleasing, mBytes}; }
        // Teardown only, after the maintenance producer/consumer have joined.
        void clear()
        {
            std::deque<Owner> released;
            { std::lock_guard lock(mMutex); released.swap(mOwners); mBytes = 0; }
        }
    private:
        struct Owner { osg::ref_ptr<const osg::Referenced> object; std::uint64_t estimate = 0; };
        mutable std::mutex mMutex;
        std::deque<Owner> mOwners;
        std::size_t mMaximumOwners, mReleasing = 0;
        std::uint64_t mMaximumBytes, mBytes = 0;
    };
}
#endif
