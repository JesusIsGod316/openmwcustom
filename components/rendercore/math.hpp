#ifndef OPENMW_COMPONENTS_RENDERCORE_MATH_H
#define OPENMW_COMPONENTS_RENDERCORE_MATH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace RenderCore
{
    using WorldPosition = glm::dvec3;
    using LocalPosition = glm::vec3;
    using LocalScale = glm::vec3;
    using Rotation = glm::quat;
    using Color = glm::vec4;

    struct LocalTransform
    {
        LocalPosition translation{ 0.0f, 0.0f, 0.0f };
        Rotation rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        LocalScale scale{ 1.0f, 1.0f, 1.0f };
    };

    struct WorldTransform
    {
        WorldPosition translation{ 0.0, 0.0, 0.0 };
        Rotation rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        LocalScale scale{ 1.0f, 1.0f, 1.0f };
    };

    struct AxisAlignedBounds
    {
        LocalPosition minimum{ 0.0f, 0.0f, 0.0f };
        LocalPosition maximum{ 0.0f, 0.0f, 0.0f };
    };
}

#endif
