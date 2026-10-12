#ifndef OPENMW_RESOURCE_P9DELETEADMISSION_H
#define OPENMW_RESOURCE_P9DELETEADMISSION_H

#include <algorithm>
#include <cmath>
#include <limits>

namespace Resource
{
    // Schedule an existing OSG deletion flush; do not change its ownership or
    // promise to preempt a driver call. A finite frame bound guarantees service
    // even under persistent render load. Pressure bypasses deferral entirely.
    class P9DeleteAdmission
    {
    public:
        enum class Reason { Disabled, Headroom, Pressure, Progress, Deferred, RepeatedFrame, UnknownFrame };
        struct Decision { bool admitted = false; Reason reason = Reason::Deferred; unsigned deferredFrames = 0; };

        Decision select(const void* context, unsigned frame, double availableMs, double predictedMs, bool suppressed,
            bool pressureOrUnknown, unsigned maximumDeferral)
        {
            if (mStarted && context == mContext && frame == mLastFrame)
            {
                // Pressure can arrive after an earlier deferred traversal. Only
                // a completed admission, not a deferral, consumes this frame.
                if (!mAdmittedThisFrame && pressureOrUnknown)
                    return admitted(Reason::Pressure);
                return { false, Reason::RepeatedFrame, mDeferredFrames };
            }
            // Context recreation / viewer reset must not carry old frame ages.
            if (mStarted && (context != mContext || frame < mLastFrame))
                mDeferredFrames = 0;
            mContext = context;
            mLastFrame = frame;
            mStarted = true;
            mAdmittedThisFrame = false;
            if (pressureOrUnknown)
                return admitted(Reason::Pressure);
            if (mDeferredFrames >= std::max(1u, maximumDeferral))
                return admitted(Reason::Progress);
            if (!suppressed && std::isfinite(availableMs) && std::isfinite(predictedMs)
                && availableMs >= std::max(0.0, predictedMs))
                return admitted(Reason::Headroom);
            if (mDeferredFrames != std::numeric_limits<unsigned>::max())
                ++mDeferredFrames;
            return { false, Reason::Deferred, mDeferredFrames };
        }

        static const char* name(Reason reason)
        {
            switch (reason)
            {
                case Reason::Disabled: return "control";
                case Reason::Headroom: return "headroom";
                case Reason::Pressure: return "pressure_or_unknown";
                case Reason::Progress: return "bounded_progress";
                case Reason::Deferred: return "deferred";
                case Reason::RepeatedFrame: return "repeated_frame";
                case Reason::UnknownFrame: return "unknown_frame_control";
            }
            return "unknown";
        }

    private:
        Decision admitted(Reason reason)
        {
            const auto deferred = mDeferredFrames;
            mDeferredFrames = 0;
            mAdmittedThisFrame = true;
            return { true, reason, deferred };
        }
        unsigned mLastFrame = 0, mDeferredFrames = 0;
        const void* mContext = nullptr;
        bool mStarted = false;
        bool mAdmittedThisFrame = false;
    };
}
#endif
