#include <components/render/backend/vsg/locallightplan.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C local-light plan failure: " << message << '\n';
        return condition;
    }

    RenderCore::LightHandle addLight(RenderCore::RenderWorld& world, RenderCore::LightRecord record)
    {
        const auto handle = world.reserveLight();
        if (!handle || !world.commit(*handle, std::move(record)))
            return {};
        return *handle;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::LightRecord constant;
    constant.position = { 1.0, 2.0, 3.0 };
    const RenderCore::LightHandle first = addLight(world, constant);

    RenderCore::LightRecord authored;
    authored.position = { 4.0, 5.0, 6.0 };
    authored.modulation = RenderCore::LightModulation::PulseSlow;
    authored.semanticFlags = RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Spot);
    authored.enabled = false;
    const RenderCore::LightHandle second = addLight(world, authored);

    const RenderVsg::LocalLightWorldPlan plan = RenderVsg::buildLocalLightWorldPlan(world);
    if (!require(first.valid() && second.valid(), "light fixtures")
        || !require(plan.valid() && plan.lights.size() == 2, "deterministic complete discovery")
        || !require(plan.lights[0].light == first && plan.lights[1].light == second, "slot-order traversal")
        || !require(plan.modulatedLights == 1 && plan.spotLights == 1 && plan.disabledLights == 1,
            "authored category accounting")
        || !require(!plan.constantPointRealizationCompatible(), "unsupported categories fail closed")
        || !require(RenderVsg::localLightPlanCurrent(world, plan.lights[0]), "current revision accepted"))
        return EXIT_FAILURE;

    RenderCore::LightRecord updated = *world.get(first);
    updated.revision = RenderCore::ResourceRevision{ 2 };
    updated.position.x = 9.0;
    if (!require(world.update(first, std::move(updated)), "light revision update")
        || !require(!RenderVsg::localLightPlanCurrent(world, plan.lights[0]), "stale plan rejected"))
        return EXIT_FAILURE;

    const RenderVsg::LocalLightWorldPlan refreshed = RenderVsg::buildLocalLightWorldPlan(world);
    if (!require(refreshed.lights[0].revision == RenderCore::ResourceRevision{ 2 }
            && refreshed.lights[0].record.position.x == 9.0,
            "updated semantics retained"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C local-light planning: PASS\n";
    return EXIT_SUCCESS;
}
