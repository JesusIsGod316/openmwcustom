#ifndef OPENMW_SCENEUTIL_UNCHANGEDTRANSFORM_H
#define OPENMW_SCENEUTIL_UNCHANGEDTRANSFORM_H
#include <atomic>
#include <cmath>
#include <cstdint>

namespace SceneUtil::UnchangedTransform
{
    inline std::atomic_bool enabled{false}; // startup configuration only
    inline std::atomic_uint64_t dirtyCallsAvoided{0}, changedInputs{0};
    template<class Vector>
    bool reuse(const Vector& old, const Vector& next) noexcept
    {
        if (!enabled.load(std::memory_order_relaxed)) return false;
        for (unsigned i = 0; i < Vector::num_components; ++i)
        {
            // Keep signed-zero assignment and nonfinite dirtying identical to
            // the control. Approximate equality never authorizes bound reuse.
            if (!std::isfinite(old[i]) || !std::isfinite(next[i]) || old[i] != next[i]
                || std::signbit(old[i]) != std::signbit(next[i]))
            { changedInputs.fetch_add(1, std::memory_order_relaxed); return false; }
        }
        dirtyCallsAvoided.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}
#endif
