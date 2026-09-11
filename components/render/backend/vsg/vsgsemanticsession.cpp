#include "vsgsemanticsession.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace RenderVsg
{
    std::unique_ptr<VsgSemanticSession> VsgSemanticSession::create(
        StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options)
    {
        return std::unique_ptr<VsgSemanticSession>(
            new VsgSemanticSession(std::move(textureResolver), std::move(options)));
    }

    VsgSemanticSession::VsgSemanticSession(
        StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options)
        : mPublisher(mWorld)
        , mModels(mWorld, mPublisher)
        , mCells(mWorld, mPublisher)
        , mPopulations(mWorld, mPublisher)
        , mShadowViews{ options.host.shadows.enabled, options.host.shadows.cascadeCount,
              { options.host.shadows.mapResolution, options.host.shadows.mapResolution },
              static_cast<float>(options.host.shadows.maximumDistance) }
        , mBootstrap(VsgRuntimeBootstrap::create(std::move(textureResolver), std::move(options)))
    {
        if (!mBootstrap)
            throw std::runtime_error("VSG semantic session bootstrap returned no runtime");
    }

    VsgSemanticSession::~VsgSemanticSession()
    {
        waitIdle();
    }

    RenderCore::RenderFrameResult VsgSemanticSession::renderFrame(const RenderCore::SingleViewFrameInput& input)
    {
        if (!mHealthy)
            return RenderCore::RenderFrameResult::Failed;
        mLastDiagnostic.clear();
        const RenderCore::StaticPopulationPublishStatus populationStatus = mPopulations.flush();
        if (populationStatus != RenderCore::StaticPopulationPublishStatus::Applied
            && populationStatus != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
            return fail("static population publication failed at the frame boundary");
        RenderCore::SingleViewFrameInput routedInput = input;
        routedInput.shadowViews = mShadowViews;
        std::optional<RenderCore::FrameRenderState> frame = mFrames.prepare(mWorld, routedInput);
        if (!frame)
            return fail("semantic frame producer rejected the engine frame input");

        const RenderCore::RenderFrameResult result = mBootstrap->renderer().renderFrame(mWorld, *frame);
        mLastDiagnostic = mBootstrap->renderer().lastDiagnostic();
        if (result == RenderCore::RenderFrameResult::Presented && !mFrames.commitPresented(*frame))
        {
            // Presentation already happened, so synchronize before poisoning
            // the route. This should be unreachable for a prepared frame, but
            // must not leave later lifetime/history state ambiguous.
            waitIdle();
            return fail("presented semantic frame could not commit frame history");
        }
        if (result == RenderCore::RenderFrameResult::Failed)
            return fail(mLastDiagnostic.empty() ? "VSG runtime rejected the semantic frame" : mLastDiagnostic);
        return result;
    }

    RenderCore::RenderFrameResult VsgSemanticSession::fail(std::string diagnostic)
    {
        if (mHealthy)
        {
            mHealthy = false;
            mLastDiagnostic = std::move(diagnostic);
        }
        return RenderCore::RenderFrameResult::Failed;
    }

    bool VsgSemanticSession::resetWorld()
    {
        if (!mHealthy)
            return false;
        waitIdle();
        if (!mWorld.reset())
        {
            mLastDiagnostic = "semantic world epoch exhausted during reset";
            return false;
        }
        mLastDiagnostic.clear();
        return true;
    }

    void VsgSemanticSession::waitIdle()
    {
        if (mBootstrap)
            mBootstrap->renderer().waitIdle();
    }
}
