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
    if (!require(producer.prepare(world, underwater).has_value(), "underwater frame rejected"))
        return EXIT_FAILURE;

    std::cout << "V4 CP4E water/auxiliary route: PASS\n";
    return EXIT_SUCCESS;
}
