#ifndef OPENMW_SCENEUTIL_GROUNDCOVERLOD2POLICY_H
#define OPENMW_SCENEUTIL_GROUNDCOVERLOD2POLICY_H

#include "groundcoverpolicy.hpp"

namespace SceneUtil::GroundcoverLod2
{
    // More useful early engagement without adding mutable draw commands or VBOs.
    inline constexpr std::array<float, 4> TierLimits{ 1.f, .87f, .70f, .52f };
    inline constexpr std::size_t RadiusBuckets = 32;

    inline float radiusForBucket(std::size_t bucket)
    {
        return std::exp2((static_cast<float>(bucket) - 8.f) * .5f);
    }

    inline std::size_t radiusBucket(float radius)
    {
        if (!std::isfinite(radius) || radius < 0.f)
            return RadiusBuckets; // Explicit shared full-detail fallback.
        for (std::size_t i = 0; i < RadiusBuckets; ++i)
            if (radius <= radiusForBucket(i))
                return i;
        return RadiusBuckets;
    }

    inline GroundcoverPolicy::Options effectiveOptions(GroundcoverPolicy::Options options, float viewDistance)
    {
        // Respect the actual fade end. Never shrink the requested near-protection zone.
        // A short view with no useful thinning interval stays full density.
        if (std::isfinite(viewDistance) && viewDistance > 0.f)
            options.farDistance = std::min(options.farDistance, .9f * viewDistance);
        if (options.farDistance <= options.nearDistance + 512.f)
            options.minimumDensity = 1.f;
        options.farDistance = std::max(options.farDistance, options.nearDistance + 1.f);
        return options;
    }

    inline unsigned desiredTier(float upperDensity)
    {
        if (!std::isfinite(upperDensity)) return 0;
        for (unsigned i = 3; i > 0; --i)
            if (upperDensity + GroundcoverPolicy::FadeWidth + .0001f < TierLimits[i])
                return i;
        return 0;
    }

    inline unsigned hystereticTier(unsigned previous, unsigned desired, float upperDensity)
    {
        previous = std::min(previous, 3u);
        desired = std::min(desired, 3u);
        if (desired <= previous) return desired;
        return upperDensity + GroundcoverPolicy::FadeWidth + GroundcoverPolicy::Hysteresis < TierLimits[desired]
            ? desired : previous;
    }

    inline std::size_t tierCount(std::span<const GroundcoverPolicy::Instance> sorted, unsigned tier)
    {
        const float limit = TierLimits[std::min(tier, 3u)];
        return static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), limit,
            [](const auto& value, float cutoff) { return value.rank < cutoff; }) - sorted.begin());
    }
}
#endif
