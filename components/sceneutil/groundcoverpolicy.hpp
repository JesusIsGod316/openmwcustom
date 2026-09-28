#ifndef OPENMW_SCENEUTIL_GROUNDCOVERPOLICY_H
#define OPENMW_SCENEUTIL_GROUNDCOVERPOLICY_H

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace SceneUtil::GroundcoverPolicy
{
    struct Instance
    {
        std::array<float, 3> position{};
        std::array<float, 3> rotation{};
        float scale = 1.f;
        std::uint64_t identity = 0;
        float rank = 0.f;
    };

    struct Options
    {
        std::size_t minInstances = 64;
        std::size_t maxLeaves = 8;
        std::size_t maxExtraDraws = 16;
        float minExtent = 512.f;
        float nearDistance = 5000.f;
        float farDistance = 13000.f;
        float minimumDensity = 0.45f;
    };

    // Each lower representation is a prefix of the same immutable instance arrays.
    // The 2% guard allows hysteresis without retaining 77% forever at the 50% floor.
    inline constexpr std::array<float, 3> TierLimits{ 1.f, 0.77f, 0.52f };
    inline constexpr float FadeWidth = 0.05f;
    inline constexpr float Hysteresis = 0.015f;

    inline std::uint64_t mix(std::uint64_t x)
    {
        x ^= x >> 30;
        x *= UINT64_C(0xbf58476d1ce4e5b9);
        x ^= x >> 27;
        x *= UINT64_C(0x94d049bb133111eb);
        return x ^ (x >> 31);
    }

    inline float rank(std::uint64_t identity)
    {
        // Exactly representable by both CPU floats and GLSL 1.20 attributes.
        return static_cast<float>(mix(identity) >> 40) * (1.f / 16777216.f);
    }

    inline bool valid(const Instance& value)
    {
        return std::isfinite(value.scale) && value.scale > 0.f
            && std::all_of(value.position.begin(), value.position.end(), [](float x) { return std::isfinite(x); })
            && std::all_of(value.rotation.begin(), value.rotation.end(), [](float x) { return std::isfinite(x); });
    }

    inline std::array<float, 3> rotate(const std::array<float, 3>& p, const std::array<float, 3>& a)
    {
        // Literal established groundcover GLSL columns, not an OSG quaternion reinterpretation.
        const float sx = std::sin(a[0]), cx = std::cos(a[0]);
        const float sy = std::sin(a[1]), cy = std::cos(a[1]);
        const float sz = std::sin(a[2]), cz = std::cos(a[2]);
        return { (cz * cy + sx * sy * sz) * p[0] + (sz * cy + cz * sx * sy) * p[1] - sy * cx * p[2],
            -sz * cx * p[0] + cz * cx * p[1] + sx * p[2],
            (cz * sy + sz * sx * cy) * p[0] + (sz * sy - cz * sx * cy) * p[1] + cx * cy * p[2] };
    }

    inline float smooth(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    inline float density(float distance, float projectedRadius, const Options& options)
    {
        if (!std::isfinite(distance) || !std::isfinite(projectedRadius)
            || options.nearDistance <= 0.f || options.farDistance <= options.nearDistance)
            return 1.f;
        const float distanceDensity = 1.f - (1.f - options.minimumDensity)
            * smooth(options.nearDistance, options.farDistance, distance);
        const float protectProminent = smooth(0.008f, 0.025f, projectedRadius);
        return distanceDensity + (1.f - distanceDensity) * protectProminent;
    }

    inline unsigned int desiredTier(float upperDensity)
    {
        if (!std::isfinite(upperDensity))
            return 0;
        for (unsigned int i = 2; i > 0; --i)
            if (upperDensity + FadeWidth + 0.0001f < TierLimits[i])
                return i;
        return 0;
    }

    inline unsigned int hystereticTier(unsigned int previous, unsigned int desired, float upperDensity)
    {
        previous = std::min(previous, 2u);
        desired = std::min(desired, 2u);
        if (desired <= previous)
            return desired; // Refine immediately: hysteresis may only retain MORE geometry.
        return upperDensity + FadeWidth + Hysteresis < TierLimits[desired] ? desired : previous;
    }

    inline float opacity(float valueRank, float valueDensity)
    {
        return std::clamp((valueDensity + FadeWidth - valueRank) / FadeWidth, 0.f, 1.f);
    }

    inline float windMargin(float wind)
    {
        if (!std::isfinite(wind))
            return std::numeric_limits<float>::infinity();
        const double w = wind;
        const double v = std::sqrt(2.0 * w * w + 1.0);
        // Each sine/cosine is bounded by one; full wind bounds reduced wind too.
        // Stomp is bounded by STOMP_DISTANCE (max 60), including height-sensitive mode.
        const double result = std::abs(2.0 * w + 0.1)
            * (std::abs(1.0 - .10 * v) + std::abs(1.0 - .04 * v)
                + std::abs(1.0 + .14 * v) + std::abs(1.0 + .28 * v)) + 60.0;
        return result < std::numeric_limits<float>::max()
            ? static_cast<float>(result + 1.0) : std::numeric_limits<float>::infinity();
    }

    using Partition = std::vector<std::size_t>;
    inline std::vector<Partition> partition(std::span<const Instance> instances, const Options& options,
        std::size_t drawables, bool split)
    {
        std::vector<Partition> leaves(1);
        leaves.front().resize(instances.size());
        std::iota(leaves.front().begin(), leaves.front().end(), 0);
        if (!split || drawables == 0)
            return leaves;
        const std::size_t limit = std::min(options.maxLeaves, 1 + options.maxExtraDraws / drawables);
        for (std::size_t n = 0; n < leaves.size() && leaves.size() < limit;)
        {
            auto& ids = leaves[n];
            std::array<float, 2> low{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            std::array<float, 2> high{ -low[0], -low[1] };
            for (const auto id : ids)
                for (unsigned int axis = 0; axis < 2; ++axis)
                {
                    low[axis] = std::min(low[axis], instances[id].position[axis]);
                    high[axis] = std::max(high[axis], instances[id].position[axis]);
                }
            const unsigned int axis = (high[0] - low[0] >= high[1] - low[1]) ? 0 : 1;
            if (ids.size() < 2 * options.minInstances || high[axis] - low[axis] < options.minExtent)
            {
                ++n;
                continue;
            }
            // Stable spatial median avoids empty subdivisions and is deterministic on ties.
            std::stable_sort(ids.begin(), ids.end(), [&](std::size_t a, std::size_t b) {
                const auto pa = instances[a].position[axis], pb = instances[b].position[axis];
                return pa != pb ? pa < pb : instances[a].identity < instances[b].identity;
            });
            const auto middle = ids.begin() + static_cast<std::ptrdiff_t>(ids.size() / 2);
            Partition other(middle, ids.end());
            ids.erase(middle, ids.end());
            leaves.push_back(std::move(other));
        }
        return leaves;
    }

    inline void sortRanks(std::vector<Instance>& instances)
    {
        std::stable_sort(instances.begin(), instances.end(), [](const Instance& a, const Instance& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.identity < b.identity;
        });
    }

    inline std::size_t tierCount(std::span<const Instance> sorted, unsigned int tier)
    {
        const float limit = TierLimits[std::min(tier, 2u)];
        return static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), limit,
            [](const Instance& value, float cutoff) { return value.rank < cutoff; }) - sorted.begin());
    }
}
#endif
