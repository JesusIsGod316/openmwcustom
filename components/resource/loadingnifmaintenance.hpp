#ifndef OPENMW_COMPONENTS_RESOURCE_LOADINGNIFMAINTENANCE_H
#define OPENMW_COMPONENTS_RESOURCE_LOADINGNIFMAINTENANCE_H

#include "openglpressure.hpp"

#include <components/sceneutil/workqueue.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Resource
{
    // One admission on the existing idle cache worker. This grants no parser,
    // scene/image eviction, new allocation or total-process memory guarantee.
    class LoadingNifMaintenancePolicy
    {
    public:
        static constexpr std::uint64_t CadenceMilliseconds = 100;
        static constexpr std::uint64_t MaximumSampleAgeMilliseconds = 1500;
        enum class Refusal : unsigned char
        { None, Disabled, Inactive, MissingQueue, Sample, Healthy, Busy, Cadence, PhaseExhausted };
        struct Admission
        {
            Refusal refusal = Refusal::Disabled;
            std::uint64_t phase = 0;
            explicit operator bool() const noexcept { return refusal == Refusal::None && phase != 0; }
        };

        explicit LoadingNifMaintenancePolicy(bool enabled = false) : mEnabled(enabled) {}
        bool enabled() const noexcept { return mEnabled; }
        // UI-owner transitions. Nested loading is resolved by LoadingScreen.
        void setLoading(bool loading) noexcept
        {
            if (mPhaseExhausted.load(std::memory_order_acquire)) return;
            auto state = mPhaseState.load(std::memory_order_acquire);
            do
            {
                if ((state & 1) == static_cast<std::uint64_t>(loading)) return;
                if (state > std::numeric_limits<std::uint64_t>::max() - 2)
                {
                    mPhaseExhausted.store(true, std::memory_order_release);
                    mPhaseState.store(0, std::memory_order_release);
                    return; // Never recycle a phase identity after exhaustion.
                }
                const auto next = ((state + 2) & ~std::uint64_t{ 1 }) | static_cast<std::uint64_t>(loading);
                if (mPhaseState.compare_exchange_weak(state, next, std::memory_order_acq_rel)) return;
            } while (true);
        }
        static bool fresh(const OpenGlPressureSample& sample, std::uint64_t now) noexcept
        {
            return sample.generation != 0 && sample.sampledAtMs != 0 && now >= sample.sampledAtMs
                && now - sample.sampledAtMs <= MaximumSampleAgeMilliseconds;
        }
        static bool observedPressure(const OpenGlPressureSample& sample) noexcept
        {
            if (sample.decision.state != OpenGlPressure::Caution
                && sample.decision.state != OpenGlPressure::Critical)
                return false;
            const auto& m = sample.memory;
            const auto& limits = sample.decision.limits;
            const bool physical = m.physicalValid && m.physicalTotal != 0
                && m.physicalAvailable <= m.physicalTotal && limits.physicalReserve != 0
                && m.physicalAvailable < limits.physicalReserve;
            const bool processCommit = m.commitValid && limits.commitReserve != 0
                && m.commitAvailable < limits.commitReserve;
            const bool systemCommit = m.systemCommitValid && limits.commitReserve != 0
                && m.systemCommitAvailable < limits.commitReserve;
            return physical || processCommit || systemCommit || (m.lowMemoryValid && m.lowMemory);
        }
        Admission tryBegin(const OpenGlPressureSample& sample, std::uint64_t now, bool queueAvailable) noexcept
        {
            if (!mEnabled) return { Refusal::Disabled };
            if (mPhaseExhausted.load(std::memory_order_acquire)) return { Refusal::PhaseExhausted };
            const auto phase = mPhaseState.load(std::memory_order_acquire);
            if ((phase & 1) == 0) return { Refusal::Inactive };
            if (!queueAvailable) return { Refusal::MissingQueue };
            if (!fresh(sample, now)) return { Refusal::Sample };
            if (!observedPressure(sample)) return { Refusal::Healthy };
            if (phase == 0) return { Refusal::Inactive }; // fail closed on phase wrap
            std::uint64_t expected = 0;
            if (!mPendingPhase.compare_exchange_strong(expected, phase, std::memory_order_acq_rel))
                return { Refusal::Busy };
            const auto last = mLastAdmissionMs.load(std::memory_order_relaxed);
            if (last != 0 && (now < last || now - last < CadenceMilliseconds))
            {
                finish(phase);
                return { Refusal::Cadence };
            }
            // A phase may have ended between the two observations. Publication
            // never converts an old loading phase into permission for a new one.
            if (phase != mPhaseState.load(std::memory_order_acquire))
            {
                finish(phase);
                return { Refusal::Inactive };
            }
            mLastAdmissionMs.store(now, std::memory_order_relaxed);
            return { Refusal::None, phase };
        }
        bool canRun(std::uint64_t phase, const OpenGlPressureSample& sample, std::uint64_t now) const noexcept
        {
            return mEnabled && !mPhaseExhausted.load(std::memory_order_acquire)
                && (phase & 1) != 0 && mPhaseState.load(std::memory_order_acquire) == phase
                && mPendingPhase.load(std::memory_order_acquire) == phase
                && fresh(sample, now) && observedPressure(sample);
        }
        void finish(std::uint64_t phase) noexcept
        {
            if (phase != 0)
                mPendingPhase.compare_exchange_strong(phase, 0, std::memory_order_acq_rel);
        }
        bool pending() const noexcept { return mPendingPhase.load(std::memory_order_acquire) != 0; }
        enum class Outcome : unsigned char { Completed, Skipped, Error };
        // Exact production optional-task boundary. Exceptions must not escape
        // into WorkQueue's required-task rethrow policy, and every owned
        // admission retires even when querying or diagnostics fails.
        template <class Operation>
        Outcome runOwned(std::uint64_t phase, Operation&& operation) noexcept
        {
            if (phase == 0 || mPendingPhase.load(std::memory_order_acquire) != phase)
                return Outcome::Skipped;
            Outcome outcome = Outcome::Error;
            try { outcome = std::forward<Operation>(operation)(); }
            catch (...) {}
            finish(phase);
            return outcome;
        }

    private:
        friend struct LoadingNifMaintenanceTestAccess;
        const bool mEnabled;
        std::atomic<bool> mPhaseExhausted{ false };
        // Active bit and generation are published together; a worker cannot
        // observe a new active phase paired with the previous phase identity.
        std::atomic<std::uint64_t> mPhaseState{ 2 }, mPendingPhase{ 0 }, mLastAdmissionMs{ 0 };
    };

    // UI-owner queue attachment and publication for the actual production
    // loading item. The borrowed queue owner must detach this owner BEFORE
    // stopping its queue: WorkQueue::stop drops queued items without signalDone.
    // Holding one queue/item reference cannot by itself override that policy.
    class LoadingNifMaintenanceQueueOwner
    {
    public:
        explicit LoadingNifMaintenanceQueueOwner(LoadingNifMaintenancePolicy& policy) : mPolicy(policy) {}
        ~LoadingNifMaintenanceQueueOwner() { detach(); }
        LoadingNifMaintenanceQueueOwner(const LoadingNifMaintenanceQueueOwner&) = delete;
        LoadingNifMaintenanceQueueOwner& operator=(const LoadingNifMaintenanceQueueOwner&) = delete;
        void attach(SceneUtil::WorkQueue* queue)
        {
            if (mQueue && mQueue.get() != queue)
                throw std::logic_error("Loading NIF maintenance queue owner changed without detach");
            mQueue = queue;
        }
        bool available() const noexcept { return mQueue.valid(); }
        bool busy() const noexcept { return mItem && !mItem->isDone(); }
        void publish(osg::ref_ptr<SceneUtil::WorkItem> item)
        {
            if (!mQueue || !item || item->isDone() || busy())
                throw std::logic_error("Loading NIF maintenance item is not publishable");
            mItem = std::move(item);
            try { mQueue->addWorkItem(mItem); }
            catch (...) { mItem = nullptr; throw; } // API throws only BEFORE publication.
        }
        bool detach(SceneUtil::WorkQueue* expected = nullptr)
        {
            if (expected && mQueue.get() != expected) return false;
            mPolicy.setLoading(false);
            if (mItem)
            {
                mItem->waitTillDone(); // Shutdown only; never called by a loading frame.
                mItem = nullptr;
            }
            mQueue = nullptr;
            return true;
        }
    private:
        LoadingNifMaintenancePolicy& mPolicy;
        osg::ref_ptr<SceneUtil::WorkQueue> mQueue;
        osg::ref_ptr<SceneUtil::WorkItem> mItem;
    };
}

#endif
