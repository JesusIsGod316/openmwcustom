#ifndef OPENMW_MWPHYSICS_PHYSICSTRACE_H
#define OPENMW_MWPHYSICS_PHYSICSTRACE_H

#include <components/debug/hitchtrace.hpp>

#include <cstdint>
#include <utility>

namespace MWPhysics
{
    // Published with the scheduler's existing simulation ownership and worker
    // wakeup. No diagnostic atomics participate in physics synchronization.
    struct PhysicsTraceCycle
    {
        std::uint64_t mEnqueue = 0;
        unsigned mFrame = 0;
        Debug::HitchTrace::Clock::time_point mQueued{};
        bool mCompleted = false;

        void begin(unsigned frame, std::size_t simulations, unsigned steps, unsigned workers)
        {
            if (!Debug::HitchTrace::requested()) return;
            Debug::HitchTrace::Scope enqueue("physics.cycle.enqueue", {}, {}, frame,
                Debug::HitchTrace::CpuTiming::WallEnvelope);
            enqueue.retain();
            enqueue.setMetric(0, "simulations", simulations);
            enqueue.setMetric(1, "steps", steps);
            enqueue.setMetric(2, "workers", workers);
            mEnqueue = enqueue.id();
            mFrame = frame;
            mQueued = Debug::HitchTrace::Clock::now();
            mCompleted = false;
        }
    };

    class PhysicsTraceStage : public Debug::HitchTrace::Scope
    {
    public:
        PhysicsTraceStage(const PhysicsTraceCycle& cycle, std::string_view phase,
            Debug::HitchTrace::CpuTiming timing = Debug::HitchTrace::CpuTiming::WallEnvelope)
            : Scope(phase, {}, {}, cycle.mFrame, timing)
        {
            setLink(cycle.mEnqueue);
        }
    };

    // Barrier wall includes the once-per-step callback on its executing worker.
    // The callback's child spans distinguish that work from other workers'
    // rendezvous waits; inclusive wall is never an additive cross-thread cost.
    template <class Barrier, class Callback>
    void tracedPhysicsBarrier(const PhysicsTraceCycle& cycle, std::string_view phase, Barrier& barrier,
        Callback&& callback)
    {
        PhysicsTraceStage wait(cycle, phase);
        barrier.wait(std::forward<Callback>(callback));
    }
}

#endif
