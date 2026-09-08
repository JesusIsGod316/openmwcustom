#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTBUFFER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTBUFFER_H

#include "locallightplan.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace RenderVsg
{
    // Backend-private storage contract for OpenMW point lights. Stock VSG point
    // lights encode inverse-square falloff, so using them would silently replace
    // the content/fallback-selected constant, linear, and quadratic equation.
    // These vec4-shaped fields are intentionally ready for std430/std140 upload.
    struct alignas(16) PackedLocalLight
    {
        glm::vec4 positionRadius{ 0.0f };
        glm::vec4 diffuse{ 0.0f };
        glm::vec4 specular{ 0.0f };
        glm::vec4 ambient{ 0.0f };
        // xyz = constant/linear/quadratic coefficients; w = actor fade.
        glm::vec4 attenuationFade{ 1.0f, 0.0f, 0.0f, 1.0f };
        // x = enabled, y = modulation enum, z/w = semantic flag halves.
        glm::uvec4 semantics{ 0u };
    };

    static_assert(sizeof(PackedLocalLight) == sizeof(glm::vec4) * 6u);
    static_assert(alignof(PackedLocalLight) >= 16u);

    enum class LocalLightBufferStatus : std::uint8_t
    {
        Ready,
        InvalidWorldPlan,
        UnsupportedModulation,
        UnsupportedSpotLight,
        RelativePositionOutOfRange,
        CapacityExceeded,
    };

    struct PackedLocalLightEntry
    {
        RenderCore::LightHandle light;
        RenderCore::ResourceRevision revision;
        PackedLocalLight data;
    };

    struct LocalLightBufferPlan
    {
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        glm::dvec3 coordinateOrigin{ 0.0 };
        std::vector<PackedLocalLightEntry> lights;
        LocalLightBufferStatus status = LocalLightBufferStatus::InvalidWorldPlan;

        [[nodiscard]] bool ready() const noexcept { return status == LocalLightBufferStatus::Ready; }
    };

    inline constexpr std::size_t DefaultMaximumPackedLocalLights = 4096;

    [[nodiscard]] inline bool packRelativePosition(
        const glm::dvec3& position, const glm::dvec3& origin, glm::vec3& result) noexcept
    {
        const glm::dvec3 relative = position - origin;
        const double limit = static_cast<double>(std::numeric_limits<float>::max());
        if (!std::isfinite(relative.x) || !std::isfinite(relative.y) || !std::isfinite(relative.z)
            || std::abs(relative.x) > limit || std::abs(relative.y) > limit || std::abs(relative.z) > limit)
            return false;
        result = glm::vec3(relative);
        return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
    }

    [[nodiscard]] inline LocalLightBufferPlan buildLocalLightBufferPlan(const LocalLightWorldPlan& source,
        glm::dvec3 coordinateOrigin, std::size_t capacity = DefaultMaximumPackedLocalLights)
    {
        LocalLightBufferPlan result;
        result.worldEpoch = source.worldEpoch;
        result.worldRevision = source.worldRevision;
        result.coordinateOrigin = coordinateOrigin;
        if (!source.valid())
            return result;
        if (source.modulatedLights != 0)
        {
            result.status = LocalLightBufferStatus::UnsupportedModulation;
            return result;
        }
        if (source.spotLights != 0)
        {
            result.status = LocalLightBufferStatus::UnsupportedSpotLight;
            return result;
        }
        if (source.lights.size() > capacity)
        {
            result.status = LocalLightBufferStatus::CapacityExceeded;
            return result;
        }

        result.lights.reserve(source.lights.size());
        for (const LocalLightPlan& light : source.lights)
        {
            glm::vec3 relativePosition;
            if (!packRelativePosition(light.record.position, coordinateOrigin, relativePosition))
            {
                result.lights.clear();
                result.status = LocalLightBufferStatus::RelativePositionOutOfRange;
                return result;
            }

            PackedLocalLight packed;
            packed.positionRadius = glm::vec4(relativePosition, light.record.effectiveRadius);
            packed.diffuse = light.record.diffuse;
            packed.specular = light.record.specular;
            packed.ambient = light.record.ambient;
            packed.attenuationFade = { light.record.constantAttenuation, light.record.linearAttenuation,
                light.record.quadraticAttenuation, light.record.actorFade };
            const std::uint32_t lowFlags = static_cast<std::uint32_t>(light.record.semanticFlags);
            const std::uint32_t highFlags = static_cast<std::uint32_t>(light.record.semanticFlags >> 32u);
            packed.semantics = { light.record.enabled ? 1u : 0u,
                static_cast<std::uint32_t>(light.record.modulation), lowFlags, highFlags };
            result.lights.push_back({ light.light, light.revision, packed });
        }
        result.status = LocalLightBufferStatus::Ready;
        return result;
    }

    [[nodiscard]] inline bool localLightBufferPlanCurrent(
        const RenderCore::RenderWorld& world, const LocalLightBufferPlan& plan) noexcept
    {
        if (!plan.ready() || plan.worldEpoch != world.epoch() || plan.worldRevision != world.revision())
            return false;
        for (const PackedLocalLightEntry& light : plan.lights)
        {
            const RenderCore::LightRecord* current = world.get(light.light);
            if (!current || current->revision != light.revision)
                return false;
        }
        return true;
    }

    [[nodiscard]] inline float evaluatePackedLocalLightAttenuation(
        const PackedLocalLight& light, float distance) noexcept
    {
        if (light.semantics.x == 0u || !std::isfinite(distance) || distance < 0.0f)
            return 0.0f;
        const float denominator = light.attenuationFade.x + light.attenuationFade.y * distance
            + light.attenuationFade.z * distance * distance;
        if (!(denominator > 0.0f) || !std::isfinite(denominator))
            return 0.0f;
        return light.attenuationFade.w / denominator;
    }
}

#endif
