#include <components/render/backend/vsg/locallightplan.hpp>
#include <components/render/backend/vsg/locallightbuffer.hpp>

#include <cmath>
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

    // Unsupported authored categories must be rejected before any partial GPU
    // payload is published.
    const RenderVsg::LocalLightBufferPlan unsupported
        = RenderVsg::buildLocalLightBufferPlan(refreshed, glm::dvec3(0.0));
    if (!require(unsupported.status == RenderVsg::LocalLightBufferStatus::UnsupportedModulation
            && unsupported.lights.empty(),
            "modulated/spot realization fails atomically"))
        return EXIT_FAILURE;

    RenderCore::RenderWorld compatibleWorld;
    RenderCore::LightRecord compatible;
    compatible.position = { 1000000009.0, -1999999998.0, 3000000003.0 };
    compatible.diffuse = { -0.25f, -0.5f, -0.75f, 1.0f };
    compatible.specular = {};
    compatible.constantAttenuation = 2.0f;
    compatible.linearAttenuation = 0.5f;
    compatible.quadraticAttenuation = 0.25f;
    compatible.effectiveRadius = 64.0f;
    compatible.actorFade = 0.75f;
    const RenderCore::LightHandle compatibleHandle = addLight(compatibleWorld, compatible);
    const glm::dvec3 coordinateOrigin{ 1000000000.0, -2000000000.0, 3000000000.0 };
    const RenderVsg::LocalLightBufferPlan packed = RenderVsg::buildLocalLightBufferPlan(
        RenderVsg::buildLocalLightWorldPlan(compatibleWorld), coordinateOrigin);
    if (!require(compatibleHandle.valid() && packed.ready() && packed.lights.size() == 1,
            "compatible light produces complete buffer")
        || !require(packed.lights[0].data.positionRadius == glm::vec4(9.0f, 2.0f, 3.0f, 64.0f),
            "camera-relative packing preserves large-world precision")
        || !require(packed.lights[0].data.diffuse == compatible.diffuse
                && packed.lights[0].data.specular == compatible.specular,
            "negative diffuse and disabled specular preserved")
        || !require(packed.lights[0].data.attenuationFade == glm::vec4(2.0f, 0.5f, 0.25f, 0.75f),
            "authored attenuation and actor fade preserved")
        || !require(std::abs(RenderVsg::evaluatePackedLocalLightAttenuation(packed.lights[0].data, 2.0f)
                    - 0.1875f)
                < 0.00001f,
            "OpenMW attenuation equation retained")
        || !require(RenderVsg::localLightBufferPlanCurrent(compatibleWorld, packed),
            "fresh packed buffer accepted"))
        return EXIT_FAILURE;

    RenderCore::LightRecord compatibleUpdate = *compatibleWorld.get(compatibleHandle);
    compatibleUpdate.revision = RenderCore::ResourceRevision{ 2 };
    compatibleUpdate.enabled = false;
    if (!require(compatibleWorld.update(compatibleHandle, std::move(compatibleUpdate)), "compatible revision update")
        || !require(!RenderVsg::localLightBufferPlanCurrent(compatibleWorld, packed),
            "world revision invalidates packed buffer"))
        return EXIT_FAILURE;

    const RenderVsg::LocalLightBufferPlan disabled = RenderVsg::buildLocalLightBufferPlan(
        RenderVsg::buildLocalLightWorldPlan(compatibleWorld), coordinateOrigin);
    if (!require(disabled.ready() && disabled.lights[0].data.semantics.x == 0u,
            "off-default state remains represented")
        || !require(RenderVsg::evaluatePackedLocalLightAttenuation(disabled.lights[0].data, 2.0f) == 0.0f,
            "disabled lights contribute no energy")
        || !require(RenderVsg::buildLocalLightBufferPlan(
                RenderVsg::buildLocalLightWorldPlan(compatibleWorld), coordinateOrigin, 0)
                        .status
                == RenderVsg::LocalLightBufferStatus::CapacityExceeded,
            "capacity overflow fails before publication"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C local-light planning and packing: PASS\n";
    return EXIT_SUCCESS;
}
