#ifndef OPENMW_TERRAIN_COMPOSITEPROGRESS_H
#define OPENMW_TERRAIN_COMPOSITEPROGRESS_H

#include <cstdint>

namespace Terrain
{
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
