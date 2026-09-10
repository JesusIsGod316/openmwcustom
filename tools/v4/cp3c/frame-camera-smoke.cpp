#include <components/render/backend/vsg/framecamera.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C frame camera failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    glm::mat4 authored(1.0f);
    authored[0][1] = 2.0f;
    authored[1][2] = 3.0f;
    authored[2][3] = 4.0f;
    authored[3][0] = 123456.25f;
    const vsg::dmat4 converted = RenderVsg::toVsgMatrix(authored);
    if (!require(converted(0, 1) == 2.0, "column-major row mapping")
        || !require(converted(1, 2) == 3.0, "second column mapping")
        || !require(converted(2, 3) == 4.0, "third column mapping")
        || !require(converted(3, 0) == 123456.25, "authored camera translation"))
        return EXIT_FAILURE;

    glm::dmat4 placement(1.0);
    placement[3][0] = 9007199254740000.0;
    placement[3][1] = -4000000000.125;
    const vsg::dmat4 precise = RenderVsg::toVsgMatrix(placement);
    if (!require(
            precise(3, 0) == placement[3][0] && precise(3, 1) == placement[3][1], "double-precision world placement"))
        return EXIT_FAILURE;

    RenderCore::FrameRenderStateDesc frameDesc;
    frameDesc.renderExtent = { 1280, 720 };
    frameDesc.outputExtent = frameDesc.renderExtent;
    RenderCore::FrameView frameView;
    frameView.identity = RenderCore::ViewHandle::fromParts(0, 1);
    frameView.extent = frameDesc.renderExtent;
    frameView.outputTarget = RenderCore::RenderTargetHandle::fromParts(0, 1);
    frameDesc.renderTargets.push_back(RenderCore::RenderTargetDesc{
        .identity = frameView.outputTarget,
        .kind = RenderCore::RenderTargetKind::Swapchain,
        .extent = frameDesc.outputExtent,
        .transient = false,
    });
    frameDesc.renderPasses.push_back(RenderCore::RenderPassDesc{
        .identity = RenderCore::RenderPassHandle::fromParts(0, 1),
        .view = frameView.identity,
        .output = frameView.outputTarget,
        .present = true,
    });
    frameDesc.views.push_back(frameView);
    if (!require(RenderCore::FrameRenderState(frameDesc).valid(), "explicit default projection convention"))
        return EXIT_FAILURE;
    frameDesc.views.front().current.projection.farPlane = 0.05;
    if (!require(!RenderCore::FrameRenderState(frameDesc).valid(), "invalid finite projection planes rejected"))
        return EXIT_FAILURE;
    frameDesc.views.front().current.projection.farPlane = 10000.0;
    frameDesc.views.front().current.projection.depthRange = static_cast<RenderCore::ClipDepthRange>(255);
    if (!require(!RenderCore::FrameRenderState(frameDesc).valid(), "unknown projection convention rejected"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C frame camera: PASS\n";
    return EXIT_SUCCESS;
}
