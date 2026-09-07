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
        mLastDiagnostic.clear();
        std::optional<RenderCore::FrameRenderState> frame = mFrames.produce(mWorld, input);
        if (!frame)
        {
            mLastDiagnostic = "semantic frame producer rejected the engine frame input";
            return RenderCore::RenderFrameResult::Failed;
        }

        const RenderCore::RenderFrameResult result = mBootstrap->renderer().renderFrame(mWorld, *frame);
        mLastDiagnostic = mBootstrap->renderer().lastDiagnostic();
        return result;
    }

    bool VsgSemanticSession::resetWorld()
    {
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
