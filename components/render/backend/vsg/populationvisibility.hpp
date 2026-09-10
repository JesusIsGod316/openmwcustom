#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_POPULATIONVISIBILITY_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_POPULATIONVISIBILITY_H

#include "staticworldplan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RenderVsg
{
    // Population resources remain resident while this inexpensive frame test
    // controls traversal. The test is deliberately conservative for a whole
    // chunk/model group: if any placement could be inside its authored maximum
    // distance, the shared hardware-instanced draw remains enabled.
    [[nodiscard]] inline bool populationWithinMaximumDistance(const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan, const RenderCore::WorldPosition& cameraPosition) noexcept
    {
        if (plan.placements.empty())
            return false;

        double maximumDistance = 0.0;
        for (const RenderCore::PopulationInstanceRecord& placement : plan.placements)
        {
            const double scaledDistance
                = static_cast<double>(placement.lod.maximumDistance) * static_cast<double>(placement.lod.scale);
            maximumDistance = std::max(maximumDistance, scaledDistance);
        }
        if (!std::isfinite(maximumDistance)
            || maximumDistance >= static_cast<double>(std::numeric_limits<float>::max()))
            return true;
        if (maximumDistance <= 0.0)
            return false;

        const RenderCore::ChunkRecord* chunk = world.get(plan.chunk);
        if (!chunk)
            return false;
        const glm::dvec3 minimum(chunk->bounds.minimum);
        const glm::dvec3 maximum(chunk->bounds.maximum);
        const glm::dvec3 nearest = glm::clamp(cameraPosition, minimum, maximum);
        const glm::dvec3 offset = cameraPosition - nearest;
        return glm::dot(offset, offset) <= maximumDistance * maximumDistance;
    }
}

#endif
