#ifndef OPENMW_MWRENDER_VULKANMW_REFERENCEPLACEMENT_H
#define OPENMW_MWRENDER_VULKANMW_REFERENCEPLACEMENT_H

#include <components/esm/position.hpp>
#include <components/rendercore/math.hpp>

#include <glm/gtc/quaternion.hpp>

namespace MWRender::VulkanMW
{
    [[nodiscard]] inline RenderCore::Rotation makeReferenceRotation(const ESM::Position& position) noexcept
    {
        return glm::normalize(
            glm::angleAxis(position.rot[2], glm::vec3(0.f, 0.f, -1.f))
            * glm::angleAxis(position.rot[1], glm::vec3(0.f, -1.f, 0.f))
            * glm::angleAxis(position.rot[0], glm::vec3(-1.f, 0.f, 0.f)));
    }
}

#endif
