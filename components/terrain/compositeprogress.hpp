#ifndef OPENMW_TERRAIN_COMPOSITEPROGRESS_H
#define OPENMW_TERRAIN_COMPOSITEPROGRESS_H

#include <cstdint>
#include <components/resource/speculativebudget.hpp>

namespace Terrain
{
    struct CompositePressureEvidence
    {
        static constexpr std::uint32_t MissingSampler = 1u << 9;
        Resource::OpenGlPressureSample sample;
        std::uint32_t refusal = 0;
        bool samplerPresent = false;

        static CompositePressureEvidence read(const Resource::SpeculativeScope::Context& context)
        {
            CompositePressureEvidence result;
            result.samplerPresent = context.sample != nullptr;
            if (context.sample) result.sample = context.sample(context.owner);
            result.refusal = Resource::SpeculativeBudget::newOwnerWorkRefusal(
                result.sample, Resource::SpeculativePriority::Background);
            if (!result.samplerPresent) result.refusal |= MissingSampler;
            return result;
        }
        std::uint32_t validityFlags() const noexcept
        {
            const auto& memory = sample.memory;
            return std::uint32_t(samplerPresent) | (std::uint32_t(memory.physicalValid) << 1)
                | (std::uint32_t(memory.commitValid) << 2) | (std::uint32_t(memory.systemCommitValid) << 3)
                | (std::uint32_t(memory.lowMemoryValid) << 4) | (std::uint32_t(memory.lowMemory) << 5)
                | (std::uint32_t(sample.growthWatermarkValid) << 6);
        }
        bool usable() const noexcept { return refusal == 0; }
        std::uint64_t signature() const noexcept
        {
            return refusal | (std::uint64_t(validityFlags()) << 10)
                | (std::uint64_t(sample.decision.state) << 17)
                | (std::uint64_t(sample.decision.admissionsPerSample) << 20);
        }
    };

    // Draw-owned wait state. Notifications are generation counters, never a
    // callback into renderer locks. A bounded periodic visit catches weak GL
    // owner expiry and unsupported producer changes without a new admission.
    class CompositeProgressWait
    {
    public:
        enum class Reason { None, Dependency, Capacity, Pressure, FrameBudget };
        bool eligible(unsigned frame, std::uint64_t wake, std::uint64_t credit,
            std::uint64_t pressure) const noexcept
        {
            return mReason == Reason::None || frame < mFrame
                || wake != mWake || credit != mCredit || pressure != mPressure
                || (mReason == Reason::FrameBudget && frame != mFrame)
                || frame - mFrame >= MaintenanceFrames;
        }
        void park(Reason reason, unsigned frame, std::uint64_t wakeBeforeCheck,
            std::uint64_t creditBeforeCheck, std::uint64_t pressureBeforeCheck) noexcept
        {
            mReason = reason;
            mFrame = frame;
            // Retain the generations sampled BEFORE checking readiness. A
            // notification between check and parking must not be swallowed.
            mWake = wakeBeforeCheck;
            mCredit = creditBeforeCheck;
            mPressure = pressureBeforeCheck;
        }
        void ready() noexcept { mReason = Reason::None; }
        Reason reason() const noexcept { return mReason; }
        static constexpr unsigned MaintenanceFrames = 32;
    private:
        Reason mReason = Reason::None;
        unsigned mFrame = 0;
        std::uint64_t mWake = 0, mCredit = 0, mPressure = 0;
    };
}
#endif
