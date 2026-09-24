#include "vsgsemanticsession.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

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
        , mWaterViews{ options.host.water.enabled && (options.host.water.reflection || options.host.water.refraction),
              options.host.water.reflection, options.host.water.refraction,
              { options.host.water.targetSize, options.host.water.targetSize }, options.host.water.reflectionLodScale,
              options.host.water.refractionLodScale }
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
        return renderFrameImpl(input, false);
    }

    RenderCore::RenderFrameResult VsgSemanticSession::renderGuiFrame(const RenderCore::SingleViewFrameInput& input)
    {
        return renderFrameImpl(input, true);
    }

    RenderCore::RenderFrameResult VsgSemanticSession::renderFrameImpl(
        const RenderCore::SingleViewFrameInput& input, bool guiOnly)
    {
        if (!mHealthy)
            return RenderCore::RenderFrameResult::Failed;
        mLastDiagnostic.clear();
        // Progress refreshes occur inside cell insertion. Do not publish partial
        // population batches or realize an incomplete world to draw the GUI.
        const RenderCore::StaticPopulationPublishStatus populationStatus = guiOnly
            ? RenderCore::StaticPopulationPublishStatus::AlreadyPresent : mPopulations.flush();
        if (populationStatus != RenderCore::StaticPopulationPublishStatus::Applied
            && populationStatus != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
            return fail("static population publication failed at the frame boundary");
        std::optional<RenderCore::FrameRenderState> frame;
        {
            Debug::GameplayDiagnostics::Stage preparing("semantic_frame_prepare");
            RenderCore::SingleViewFrameInput routedInput = input;
            routedInput.shadowViews = mShadowViews;
            routedInput.waterViews = mWaterViews;
            frame = mFrames.prepare(mWorld, routedInput);
            if (Debug::GameplayDiagnostics::sampling())
                Debug::GameplayDiagnostics::recordEvent("effect_frame_handoff", {
                    {"owned_snapshot", std::to_string(bool(input.ownedImmediateEffects))},
                    {"publication_workers", std::to_string(input.ownedImmediateEffects
                        ? input.ownedImmediateEffects->publicationWorkers() : 0)},
                    {"draws", std::to_string(input.ownedImmediateEffects
                        ? input.ownedImmediateEffects->draws().size() : input.immediateEffectDraws.size())},
                    {"geometry_bytes", std::to_string(input.ownedImmediateEffects
                        ? input.ownedImmediateEffects->payloadBytes() : 0)},
                    {"valid", std::to_string(bool(frame))} });
        }
        if (!frame)
            return fail("semantic frame producer rejected the engine frame input");

        const RenderCore::RenderFrameResult result = guiOnly
            ? mBootstrap->renderer().renderGuiFrame(mWorld, *frame)
            : mBootstrap->renderer().renderFrame(mWorld, *frame);
        mLastDiagnostic = mBootstrap->renderer().lastDiagnostic();
        const bool historyCommitted = [&] {
            Debug::GameplayDiagnostics::Stage history("frame_history_commit");
            return result != RenderCore::RenderFrameResult::Presented || mFrames.commitPresented(*frame);
        }();
        if (!historyCommitted)
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
