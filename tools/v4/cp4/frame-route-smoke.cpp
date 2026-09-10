#include <components/rendercore/framerenderstate.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP4A frame route failure: " << message << '\n';
        return condition;
    }

    RenderCore::FrameRenderStateDesc makeAuxiliaryRoute()
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.renderExtent = { 1280, 720 };
        desc.outputExtent = { 1920, 1080 };

        const auto auxiliaryTarget = RenderCore::RenderTargetHandle::fromParts(0, 1);
        const auto presentTarget = RenderCore::RenderTargetHandle::fromParts(1, 1);
        const auto auxiliaryView = RenderCore::ViewHandle::fromParts(0, 1);
        const auto mainView = RenderCore::ViewHandle::fromParts(1, 1);
        const auto auxiliaryPass = RenderCore::RenderPassHandle::fromParts(0, 1);
        const auto presentPass = RenderCore::RenderPassHandle::fromParts(1, 1);

        desc.renderTargets = {
            RenderCore::RenderTargetDesc{
                .identity = auxiliaryTarget,
                .kind = RenderCore::RenderTargetKind::Offscreen,
                .extent = { 1024, 1024 },
                .colorFormat = RenderCore::RenderTargetFormat::Rgba16Float,
                .depthFormat = RenderCore::RenderTargetFormat::Depth32Float,
                .sampleCount = 1,
                .historyEpoch = RenderCore::InitialHistoryEpoch,
                .historyValid = false,
                .transient = true,
            },
            RenderCore::RenderTargetDesc{
                .identity = presentTarget,
                .kind = RenderCore::RenderTargetKind::Swapchain,
                .extent = desc.outputExtent,
                .colorFormat = RenderCore::RenderTargetFormat::SurfaceColor,
                .depthFormat = std::nullopt,
                .sampleCount = 1,
                .historyEpoch = RenderCore::InitialHistoryEpoch,
                .historyValid = false,
                .transient = false,
            },
        };

        RenderCore::FrameView reflection;
        reflection.identity = auxiliaryView;
        reflection.viewIndex = 0;
        reflection.kind = RenderCore::ViewKind::Reflection;
        reflection.outputTarget = auxiliaryTarget;
        reflection.extent = { 1024, 1024 };

        RenderCore::FrameView main;
        main.identity = mainView;
        main.viewIndex = 1;
        main.kind = RenderCore::ViewKind::Main;
        main.outputTarget = presentTarget;
        main.extent = desc.renderExtent;
        desc.views = { reflection, main };

        desc.renderPasses = {
            RenderCore::RenderPassDesc{
                .identity = auxiliaryPass,
                .view = auxiliaryView,
                .output = auxiliaryTarget,
                .inputs = {},
                .dependencies = {},
                .colorLoad = RenderCore::RenderPassLoad::Clear,
                .depthLoad = RenderCore::RenderPassLoad::Clear,
                .colorStore = RenderCore::RenderPassStore::Store,
                .depthStore = RenderCore::RenderPassStore::Store,
                .present = false,
            },
            RenderCore::RenderPassDesc{
                .identity = presentPass,
                .view = mainView,
                .output = presentTarget,
                .inputs = { auxiliaryTarget },
                .dependencies = { auxiliaryPass },
                .colorLoad = RenderCore::RenderPassLoad::Clear,
                .depthLoad = RenderCore::RenderPassLoad::Clear,
                .colorStore = RenderCore::RenderPassStore::Store,
                .depthStore = RenderCore::RenderPassStore::Store,
                .present = true,
            },
        };
        return desc;
    }
}

int main()
{
    const RenderCore::FrameRenderStateDesc valid = makeAuxiliaryRoute();
    if (!require(RenderCore::FrameRenderState(valid).valid(), "valid offscreen-to-present route rejected"))
        return EXIT_FAILURE;

    auto badTarget = valid;
    badTarget.views.back().outputTarget = RenderCore::RenderTargetHandle::fromParts(7, 1);
    if (!require(!RenderCore::FrameRenderState(badTarget).valid(), "unknown view output target accepted"))
        return EXIT_FAILURE;

    auto forwardDependency = valid;
    forwardDependency.renderPasses.front().dependencies = { forwardDependency.renderPasses.back().identity };
    if (!require(!RenderCore::FrameRenderState(forwardDependency).valid(), "forward pass dependency accepted"))
        return EXIT_FAILURE;

    auto duplicateIdentity = valid;
    duplicateIdentity.views.back().identity = duplicateIdentity.views.front().identity;
    if (!require(!RenderCore::FrameRenderState(duplicateIdentity).valid(), "duplicate stable view identity accepted"))
        return EXIT_FAILURE;

    auto weather = valid;
    weather.environment.skyColor = { 0.2f, 0.3f, 0.5f, 1.0f };
    weather.environment.nightSkyFactor = 0.4f;
    weather.environment.cloudBlendFactor = 0.6f;
    weather.environment.windDirection = { 1.0f, 0.0f, 0.0f };
    weather.environment.windSpeed = 12.0f;
    weather.environment.precipitationIntensity = 0.75f;
    weather.environment.precipitationEnabled = true;
    weather.environment.shadowsEnabled = true;
    if (!require(RenderCore::FrameRenderState(weather).valid(), "valid CP4D weather state rejected"))
        return EXIT_FAILURE;

    weather.environment.precipitationIntensity = 1.25f;
    if (!require(!RenderCore::FrameRenderState(weather).valid(), "out-of-range precipitation accepted"))
        return EXIT_FAILURE;

    std::cout << "CP4A/CP4D frame route smoke: PASS\n";
    return EXIT_SUCCESS;
}
