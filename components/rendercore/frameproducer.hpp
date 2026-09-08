#ifndef OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H

#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <optional>
#include <utility>

namespace RenderCore
{
    struct SingleViewFrameInput
    {
        CameraState camera;
        Extent2D renderExtent;
        Extent2D outputExtent;
        FrameEnvironmentState environment;
        double simulationTime = 0.0;
        double frameDelta = 0.0;
        float lodScale = 1.0f;
        glm::vec2 jitter{ 0.0f, 0.0f };
        glm::vec2 projectionOffset{ 0.0f, 0.0f };
        bool invalidateHistory = false;
    };

    // Small backend-neutral frame-boundary producer for the primary view. It
    // owns monotonic frame IDs and makes history discontinuities explicit when
    // the world epoch, render/output extents, or caller continuity changes.
    // Later temporal backends can consume the same current/previous contract;
    // the CP3C Vulkan host currently requires equal extents and zero jitter.
    class SingleViewFrameProducer final
    {
    public:
        // Prepare is transactional: a renderer may skip acquisition during
        // minimize/resize without turning an unpresented camera into temporal
        // history or consuming a semantic frame id.
        [[nodiscard]] std::optional<FrameRenderState> prepare(
            const RenderWorld& world, const SingleViewFrameInput& input) const
        {
            if (!mNextFrameId)
                return std::nullopt;

            const bool dimensionsMatch = mPreviousRenderExtent == input.renderExtent
                && mPreviousOutputExtent == input.outputExtent;
            const bool continuous = mPreviousCamera.has_value() && mPreviousWorldEpoch == world.epoch()
                && dimensionsMatch && !input.invalidateHistory;

            HistoryEpoch candidateHistoryEpoch = mHistoryEpoch;
            if (!continuous && mPreviousCamera)
            {
                const std::optional<HistoryEpoch> nextHistory = advanceMonotonic(mHistoryEpoch);
                if (!nextHistory)
                    return std::nullopt;
                candidateHistoryEpoch = *nextHistory;
            }

            FrameView view;
            view.current = input.camera;
            view.previous = continuous ? *mPreviousCamera : input.camera;
            view.extent = input.renderExtent;
            view.lodScale = input.lodScale;
            view.historyEpoch = candidateHistoryEpoch;
            view.temporal = input.jitter != glm::vec2(0.0f) || input.projectionOffset != glm::vec2(0.0f);
            view.historyValid = continuous;

            FrameRenderStateDesc desc;
            desc.frameId = *mNextFrameId;
            desc.worldEpoch = world.epoch();
            desc.renderWorldRevision = world.revision();
            desc.historyEpoch = candidateHistoryEpoch;
            desc.simulationTime = input.simulationTime;
            desc.frameDelta = input.frameDelta;
            desc.renderExtent = input.renderExtent;
            desc.outputExtent = input.outputExtent;
            desc.jitter = input.jitter;
            desc.projectionOffset = input.projectionOffset;
            desc.historyValid = continuous;
            desc.environment = input.environment;
            desc.views.push_back(std::move(view));

            FrameRenderState result(std::move(desc));
            if (!result.valid())
                return std::nullopt;

            return result;
        }

        [[nodiscard]] bool commitPresented(const FrameRenderState& frame) noexcept
        {
            if (!mNextFrameId || !frame.valid() || frame.frameId() != *mNextFrameId || frame.views().size() != 1
                || frame.views().front().extent != frame.renderExtent()
                || frame.views().front().historyEpoch != frame.historyEpoch())
                return false;

            mHistoryEpoch = frame.historyEpoch();
            mPreviousCamera = frame.views().front().current;
            mPreviousWorldEpoch = frame.worldEpoch();
            mPreviousRenderExtent = frame.renderExtent();
            mPreviousOutputExtent = frame.outputExtent();
            mNextFrameId = advanceMonotonic(*mNextFrameId);
            return true;
        }

        // Convenience for consumers whose publication is guaranteed. Runtime
        // backends with a fallible acquire/present path use prepare/commit.
        [[nodiscard]] std::optional<FrameRenderState> produce(
            const RenderWorld& world, const SingleViewFrameInput& input)
        {
            std::optional<FrameRenderState> result = prepare(world, input);
            if (!result || !commitPresented(*result))
                return std::nullopt;
            return result;
        }

        [[nodiscard]] FrameId nextFrameId() const noexcept { return mNextFrameId.value_or(FrameId{}); }
        [[nodiscard]] HistoryEpoch historyEpoch() const noexcept { return mHistoryEpoch; }

    private:
        std::optional<FrameId> mNextFrameId = InitialFrameId;
        HistoryEpoch mHistoryEpoch = InitialHistoryEpoch;
        std::optional<CameraState> mPreviousCamera;
        WorldEpoch mPreviousWorldEpoch;
        Extent2D mPreviousRenderExtent;
        Extent2D mPreviousOutputExtent;
    };
}

#endif
