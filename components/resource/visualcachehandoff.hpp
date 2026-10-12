#ifndef OPENMW_COMPONENTS_RESOURCE_VISUALCACHEHANDOFF_H
#define OPENMW_COMPONENTS_RESOURCE_VISUALCACHEHANDOFF_H

#include <components/sceneutil/workqueue.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Resource
{
    // UI-owner admission, worker-owner execution. This permits only bounded
    // cache-only retirement after an outer loading handoff. No live scene
    // mutation, GL calls, pressure fiction, parser work or new worker thread.
    class VisualCacheHandoffPolicy
    {
    public:
        static constexpr unsigned MaximumPasses = 128;
        static constexpr std::uint64_t WindowMilliseconds = 5000, CadenceMilliseconds = 16;
        struct Admission
        {
            std::uint64_t generation = 0, deadline = 0;
            explicit operator bool() const noexcept { return generation != 0; }
        };
        enum class Outcome : unsigned char { Completed, Skipped, Error };
        explicit VisualCacheHandoffPolicy(bool enabled) : mEnabled(enabled) {}
        bool enabled() const noexcept { return mEnabled; }
        // Main-thread methods; never wait for the worker on a loading frame.
        // Return the actual edge, so the caller can publish the first pass at
        // loading-off rather than waiting for Scene::update. Save/Lua startup
        // may otherwise consume this entire finite window before that update.
        bool setLoading(bool active, std::uint64_t now) noexcept
        {
            if (!mEnabled || mExhausted || active == mLoading) return false;
            mLoading = active;
            if (mGeneration == std::numeric_limits<std::uint64_t>::max())
            { mExhausted = true; mCurrent.store(0); return false; }
            ++mGeneration;
            mRemaining = active ? 0 : MaximumPasses;
            mLast = 0;
            mDeadline = now > std::numeric_limits<std::uint64_t>::max() - WindowMilliseconds
                ? 0 : now + WindowMilliseconds;
            mCurrent.store(active || !now || !mDeadline ? 0 : mGeneration, std::memory_order_release);
            return true;
        }
        Admission tryBegin(std::uint64_t now, bool queueAvailable) noexcept
        {
            if (!mEnabled || mLoading || mExhausted || !mGeneration || !mRemaining || !queueAvailable
                || !now || !mDeadline || now > mDeadline || mCurrent.load(std::memory_order_acquire) != mGeneration
                || (mLast && (now < mLast || now - mLast < CadenceMilliseconds))) return {};
            std::uint64_t expected = 0;
            if (!mPending.compare_exchange_strong(expected, mGeneration, std::memory_order_acq_rel)) return {};
            --mRemaining; mLast = now;
            return {mGeneration, mDeadline};
        }
        bool canRun(Admission token, std::uint64_t now) const noexcept
        {
            return mEnabled && token && now && now <= token.deadline
                && mCurrent.load(std::memory_order_acquire) == token.generation
                && mPending.load(std::memory_order_acquire) == token.generation;
        }
        void finish(Admission token) noexcept
        {
            if (token) mPending.compare_exchange_strong(token.generation, 0, std::memory_order_acq_rel);
        }
        void close() noexcept { mCurrent.store(0, std::memory_order_release); }
        template<class Clock, class Operation>
        Outcome runOwned(Admission token, Clock&& clock, Operation&& operation) noexcept
        {
            Outcome result = Outcome::Skipped;
            try
            {
                if (canRun(token, std::forward<Clock>(clock)()))
                { std::forward<Operation>(operation)(); result = Outcome::Completed; }
            }
            catch (...) { result = Outcome::Error; }
            finish(token);
            return result;
        }
    private:
        const bool mEnabled;
        bool mLoading = false, mExhausted = false; // UI owner only
        std::uint64_t mGeneration = 0, mDeadline = 0, mLast = 0;
        unsigned mRemaining = 0;
        std::atomic<std::uint64_t> mCurrent{0}, mPending{0};
    };
    class VisualCacheHandoffQueueOwner
    {
    public:
        explicit VisualCacheHandoffQueueOwner(VisualCacheHandoffPolicy& policy) : mPolicy(policy) {}
        ~VisualCacheHandoffQueueOwner() { detach(); }
        void attach(SceneUtil::WorkQueue* queue)
        {
            if (mQueue && mQueue.get() != queue) throw std::logic_error("Visual handoff queue changed without detach");
            mQueue = queue;
        }
        bool available() const noexcept { return mQueue.valid(); }
        bool busy() const noexcept { return mItem && !mItem->isDone(); }
        void publish(osg::ref_ptr<SceneUtil::WorkItem> item)
        {
            if (!mQueue || !item || item->isDone() || busy()) throw std::logic_error("Visual handoff item not publishable");
            mItem = std::move(item);
            try { mQueue->addWorkItem(mItem); }
            catch (...) { mItem = nullptr; throw; } // WorkQueue throws before publication.
        }
        bool detach(SceneUtil::WorkQueue* expected = nullptr)
        {
            if (expected && mQueue.get() != expected) return false;
            mPolicy.close();
            if (mItem) { mItem->waitTillDone(); mItem = nullptr; } // Shutdown only.
            mQueue = nullptr;
            return true;
        }
    private:
        VisualCacheHandoffPolicy& mPolicy;
        osg::ref_ptr<SceneUtil::WorkQueue> mQueue;
        osg::ref_ptr<SceneUtil::WorkItem> mItem;
    };
}
#endif
