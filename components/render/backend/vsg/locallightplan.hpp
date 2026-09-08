#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTPLAN_H

#include <components/rendercore/renderworld.hpp>

#include <cstdint>
#include <vector>

namespace RenderVsg
{
    struct LocalLightPlan
    {
        RenderCore::LightHandle light;
        RenderCore::ResourceRevision revision;
        RenderCore::LightRecord record;
    };

    struct LocalLightWorldPlan
    {
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        std::vector<LocalLightPlan> lights;
        std::uint32_t modulatedLights = 0;
        std::uint32_t spotLights = 0;
        std::uint32_t disabledLights = 0;

        [[nodiscard]] bool valid() const noexcept
        {
            return worldEpoch.valid() && worldRevision.valid();
        }

        // The first VSG realization may consume this only when it implements
        // every authored category it would otherwise flatten incorrectly.
        [[nodiscard]] bool constantPointRealizationCompatible() const noexcept
        {
            return valid() && modulatedLights == 0 && spotLights == 0;
        }
    };

    [[nodiscard]] inline LocalLightWorldPlan buildLocalLightWorldPlan(const RenderCore::RenderWorld& world)
    {
        LocalLightWorldPlan result;
        result.worldEpoch = world.epoch();
        result.worldRevision = world.revision();
        result.lights.reserve(world.lightCount());
        world.forEachLight([&](RenderCore::LightHandle handle, const RenderCore::LightRecord& record) {
            result.lights.push_back({ handle, record.revision, record });
            if (record.modulation != RenderCore::LightModulation::Constant)
                ++result.modulatedLights;
            if ((record.semanticFlags & RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Spot)) != 0)
                ++result.spotLights;
            if (!record.enabled)
                ++result.disabledLights;
        });
        return result;
    }

    [[nodiscard]] inline bool localLightPlanCurrent(
        const RenderCore::RenderWorld& world, const LocalLightPlan& plan) noexcept
    {
        const RenderCore::LightRecord* current = world.get(plan.light);
        return current && current->revision == plan.revision;
    }
}

#endif
