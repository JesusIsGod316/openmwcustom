#include <components/rendercore/frameproducer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP4E water/auxiliary failure: " << message << '\n';
        return condition;
    }

    [[nodiscard]] const RenderCore::FrameView* findView(
        const RenderCore::FrameRenderState& frame, RenderCore::ViewKind kind)
    {
        const auto found = std::find_if(
            frame.views().begin(), frame.views().end(), [kind](const auto& view) { return view.kind == kind; });
        return found == frame.views().end() ? nullptr : &*found;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::SingleViewFrameProducer producer;
    RenderCore::SingleViewFrameInput input;
    input.renderExtent = { 1280, 720 };
    input.outputExtent = input.renderExtent;
    input.camera.worldPosition = { 20.0, 30.0, 90.0 };
    input.camera.view = glm::translate(glm::mat4(1.0f), glm::vec3(-20.0f, -30.0f, -90.0f));
    input.environment.waterEnabled = true;
    input.environment.waterHeight = 10.0;
    input.environment.underwater = false;
    input.waterViews = { true, true, true, { 1024, 1024 }, 0.45f, 0.6f };

    RenderCore::SingleViewFrameInput::AuxiliaryView map;
    map.kind = RenderCore::ViewKind::Map;
    map.camera = input.camera;
    map.extent = { 512, 512 };
    map.lodScale = 0.75f;
    input.auxiliaryViews.push_back(map);

    const auto frame = producer.produce(world, input);
    if (!require(frame && frame->valid(), "valid water route rejected")
        || !require(frame->views().size() == 4, "main/reflection/refraction/map views emitted")
        || !require(frame->renderTargets().size() == 4, "persistent offscreen targets emitted")
        || !require(frame->renderPasses().size() == 4, "offscreen passes emitted before present"))
        return EXIT_FAILURE;

    const RenderCore::FrameView* reflection = findView(*frame, RenderCore::ViewKind::Reflection);
    const RenderCore::FrameView* refraction = findView(*frame, RenderCore::ViewKind::Refraction);
    const RenderCore::FrameView* mapView = findView(*frame, RenderCore::ViewKind::Map);
    if (!require(reflection && refraction && mapView, "typed auxiliary views retained")
        || !require(reflection->clipPlane && reflection->clipPlane->normal.z == 1.0f,
            "reflection keeps the above-water half-space")
        || !require(refraction->clipPlane && refraction->clipPlane->normal.z == -1.0f,
            "refraction keeps the below-water half-space")
        || !require(std::abs(reflection->current.worldPosition.z + 70.0) < 0.001,
            "reflection camera mirrors around water height")
        || !require(glm::determinant(glm::mat3(glm::inverse(reflection->current.view))) > 0.99f,
            "reflection camera remains right-handed for face culling")
        || !require(frame->renderPasses().back().present
                && frame->renderPasses().back().dependencies.size() == 2
                && frame->renderPasses().back().inputs.size() == 2,
            "main pass consumes both water targets")
        || !require(!frame->renderTargets()[1].transient && !frame->renderTargets()[2].transient
                && !frame->renderTargets()[3].transient,
            "water and map targets remain resident"))
        return EXIT_FAILURE;

    const RenderCore::RenderTargetHandle reflectionTarget = reflection->outputTarget;
    const RenderCore::RenderTargetHandle refractionTarget = refraction->outputTarget;

    auto dry = input;
    dry.environment.waterEnabled = false;
    dry.environment.waterHeight = 0.0;
    const auto dryFrame = producer.produce(world, dry);
    if (!require(dryFrame && dryFrame->valid(), "water-disable transition rejected")
        || !require(!findView(*dryFrame, RenderCore::ViewKind::Reflection)
                && !findView(*dryFrame, RenderCore::ViewKind::Refraction),
            "disabled water still emitted reflection/refraction work")
        || !require(findView(*dryFrame, RenderCore::ViewKind::Map) != nullptr,
            "generic map view was incorrectly tied to water enable state")
        || !require(dryFrame->historyValid(), "water-disable transition invalidated unrelated main-view history"))
        return EXIT_FAILURE;

    const auto rewetted = producer.produce(world, input);
    const RenderCore::FrameView* rewettedReflection
        = rewetted ? findView(*rewetted, RenderCore::ViewKind::Reflection) : nullptr;
    const RenderCore::FrameView* rewettedRefraction
        = rewetted ? findView(*rewetted, RenderCore::ViewKind::Refraction) : nullptr;
    if (!require(rewetted && rewetted->valid(), "water re-enable transition rejected")
        || !require(rewettedReflection && rewettedRefraction, "water views did not return after re-enable")
        || !require(rewettedReflection->outputTarget == reflectionTarget
                && rewettedRefraction->outputTarget == refractionTarget,
            "persistent water target identities changed across a dry-cell transition")
        || !require(!rewettedReflection->historyValid && !rewettedRefraction->historyValid,
            "non-temporal water views incorrectly inherited temporal history"))
        return EXIT_FAILURE;

    auto sampled = input;
    sampled.auxiliaryViews.front().sampledByMain = true;
    const auto sampledFrame = producer.prepare(world, sampled);
    if (!require(sampledFrame && sampledFrame->valid(), "sampled generic auxiliary view rejected")
        || !require(sampledFrame->renderPasses().back().present
                && sampledFrame->renderPasses().back().dependencies.size() == 3
                && sampledFrame->renderPasses().back().inputs.size() == 3,
            "sampled map target was not ordered before the main pass"))
        return EXIT_FAILURE;

    auto invalid = input;
    invalid.waterViews.reflection = false;
    invalid.waterViews.refraction = false;
    if (!require(!producer.prepare(world, invalid), "empty enabled water-view set accepted"))
        return EXIT_FAILURE;

    invalid = input;
    invalid.auxiliaryViews.front().kind = RenderCore::ViewKind::Shadow;
    if (!require(!producer.prepare(world, invalid), "derived shadow smuggled through explicit auxiliary route"))
        return EXIT_FAILURE;

    auto badPlane = input;
    badPlane.auxiliaryViews.front().clipPlane
        = RenderCore::WorldClipPlane{ glm::vec3(0.0f, 0.0f, 2.0f), 0.0 };
    if (!require(!producer.prepare(world, badPlane), "non-normalized clip plane accepted"))
        return EXIT_FAILURE;

    auto underwater = input;
    underwater.environment.underwater = true;
    underwater.environment.skyEnabled = false;
    const auto underwaterFrame = producer.prepare(world, underwater);
    if (!require(underwaterFrame.has_value(), "underwater frame rejected"))
        return EXIT_FAILURE;
    for (const bool cave : {false, true})
    {
        underwater.environment.interior = cave;
        const auto wetFrame = producer.prepare(world, underwater);
        const auto* reflected = wetFrame ? findView(*wetFrame, RenderCore::ViewKind::Reflection) : nullptr;
        const auto* refracted = wetFrame ? findView(*wetFrame, RenderCore::ViewKind::Refraction) : nullptr;
        if (!require(reflected && refracted && reflected->clipPlane && refracted->clipPlane,
                "underwater views missing")
            || !require(reflected->clipPlane->normal.z == -1 && reflected->clipPlane->distance == 10,
                "underwater reflection discarded the submerged scene")
            || !require(refracted->clipPlane->normal.z == 1 && refracted->clipPlane->distance == -10,
                "underwater refraction discarded the above-water scene"))
            return EXIT_FAILURE;
    }

    // A point on the mirror plane must project to equal Y and opposite X in
    // the right-handed reflected view, for translated/rotated/pitched cameras.
    for (float yaw : {0.0f, 0.6f, 2.0f})
    {
        auto projectedInput = input;
        const glm::vec3 eye(20,30,90);
        const glm::vec3 forward(std::sin(yaw), std::cos(yaw), -0.3f);
        projectedInput.camera.view = glm::lookAtRH(eye, eye + forward, glm::vec3(0,0,1));
        const auto projected = producer.prepare(world, projectedInput);
        const auto* reflected = projected ? findView(*projected, RenderCore::ViewKind::Reflection) : nullptr;
        if (!require(reflected != nullptr, "projected reflection missing")) return EXIT_FAILURE;
        for (float side : {-70.f, 70.f})
        {
            const glm::vec4 point(eye + forward * 300.f + glm::vec3(side,20.f,10.f), 1.f);
            glm::vec4 planePoint = point; planePoint.z = 10.f;
            const glm::vec4 mainEye = projectedInput.camera.view * planePoint;
            const glm::vec4 reflectedEye = reflected->current.view * planePoint;
            // Float lookAt rebuilds a unit forward vector at a translated eye.
            // Compare relatively: the observed ~0.001 world-unit rounding at
            // 330 units is far below a pixel and is not a handedness failure.
            if (!require(glm::length(glm::vec3(mainEye.x + reflectedEye.x,
                    mainEye.y - reflectedEye.y, mainEye.z - reflectedEye.z))
                    < 0.00002f * glm::length(glm::vec3(mainEye)),
                    "reflected-camera screen mapping changed")) return EXIT_FAILURE;
        }
    }

    std::cout << "V4 CP4E water/auxiliary route: PASS\n";
    return EXIT_SUCCESS;
}
