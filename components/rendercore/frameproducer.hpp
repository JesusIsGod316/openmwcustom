#ifndef OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H

#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace RenderCore
{
    struct DynamicTransformInput
    {
        InstanceHandle instance;
        WorldTransform transform;
    };

    struct SkeletonPoseInput
    {
        InstanceHandle instance;
        SkeletonHandle skeleton;
        std::vector<glm::mat4> localTransforms;
    };

    struct MorphWeightInput
    {
        InstanceHandle instance;
        MeshHandle mesh;
        std::optional<ModelNodeIndex> modelNode;
        std::vector<float> weights;
    };

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
        std::vector<DynamicTransformInput> dynamicTransforms;
        std::vector<SkeletonPoseInput> skeletonPoses;
        std::vector<MorphWeightInput> morphWeights;
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

            for (const DynamicTransformInput& inputTransform : input.dynamicTransforms)
            {
                const InstanceRecord* instance = world.get(inputTransform.instance);
                if (!instance)
                    return std::nullopt;
                const auto previous = std::find_if(mPreviousDynamicTransforms.begin(),
                    mPreviousDynamicTransforms.end(), [&](const DynamicTransformState& value) {
                        return value.instance == inputTransform.instance;
                    });
                const bool transformContinuous = continuous && previous != mPreviousDynamicTransforms.end()
                    && previous->instanceRevision == instance->revision;
                DynamicTransformState transform;
                transform.instance = inputTransform.instance;
                transform.instanceRevision = instance->revision;
                transform.current = inputTransform.transform;
                transform.previous = transformContinuous ? previous->current : inputTransform.transform;
                transform.historyValid = transformContinuous;
                desc.dynamicTransforms.push_back(std::move(transform));
            }

            for (const SkeletonPoseInput& inputPose : input.skeletonPoses)
            {
                const InstanceRecord* instance = world.get(inputPose.instance);
                const SkeletonRecord* skeleton = world.get(inputPose.skeleton);
                if (!instance || !instance->skeleton || *instance->skeleton != inputPose.skeleton || !skeleton
                    || !skeleton->payload || skeleton->payload->bones.empty()
                    || inputPose.localTransforms.size() != skeleton->payload->bones.size())
                    return std::nullopt;

                const auto previous = std::find_if(mPreviousSkeletonPoses.begin(), mPreviousSkeletonPoses.end(),
                    [&](const SkeletonPoseState& value) { return value.instance == inputPose.instance; });
                const bool poseContinuous = continuous && previous != mPreviousSkeletonPoses.end()
                    && previous->instanceRevision == instance->revision && previous->skeleton == inputPose.skeleton
                    && previous->skeletonRevision == skeleton->revision
                    && previous->current.size() == inputPose.localTransforms.size();

                SkeletonPoseState pose;
                pose.instance = inputPose.instance;
                pose.instanceRevision = instance->revision;
                pose.skeleton = inputPose.skeleton;
                pose.skeletonRevision = skeleton->revision;
                pose.current = inputPose.localTransforms;
                pose.previous = poseContinuous ? previous->current : inputPose.localTransforms;
                pose.historyValid = poseContinuous;
                desc.skeletonPoses.push_back(std::move(pose));
            }

            for (const MorphWeightInput& inputMorph : input.morphWeights)
            {
                const InstanceRecord* instance = world.get(inputMorph.instance);
                const MeshRecord* mesh = world.get(inputMorph.mesh);
                if (!instance || !mesh || !mesh->morphed || !mesh->morphs
                    || inputMorph.weights.size() != mesh->morphs->targets.size()
                    || !instanceOwnsMesh(world, *instance, inputMorph))
                    return std::nullopt;

                const auto previous = std::find_if(mPreviousMorphWeights.begin(), mPreviousMorphWeights.end(),
                    [&](const MorphWeightState& value) {
                        return value.instance == inputMorph.instance && value.modelNode == inputMorph.modelNode;
                    });
                const bool morphContinuous = continuous && previous != mPreviousMorphWeights.end()
                    && previous->instanceRevision == instance->revision && previous->mesh == inputMorph.mesh
                    && previous->meshRevision == mesh->revision && previous->current.size() == inputMorph.weights.size();

                MorphWeightState morph;
                morph.instance = inputMorph.instance;
                morph.instanceRevision = instance->revision;
                morph.mesh = inputMorph.mesh;
                morph.meshRevision = mesh->revision;
                morph.modelNode = inputMorph.modelNode;
                morph.current = inputMorph.weights;
                morph.previous = morphContinuous ? previous->current : inputMorph.weights;
                morph.historyValid = morphContinuous;
                desc.morphWeights.push_back(std::move(morph));
            }

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
            mPreviousDynamicTransforms = frame.dynamicTransforms();
            mPreviousSkeletonPoses = frame.skeletonPoses();
            mPreviousMorphWeights = frame.morphWeights();
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
        [[nodiscard]] static bool instanceOwnsMesh(
            const RenderWorld& world, const InstanceRecord& instance, const MorphWeightInput& morph) noexcept
        {
            if (!morph.modelNode)
                return !instance.model && instance.mesh == morph.mesh;
            if (!instance.model)
                return false;
            const ModelRecord* model = world.get(*instance.model);
            if (!model || !model->payload || morph.modelNode->value() >= model->payload->nodes.size())
                return false;
            const ModelNodeRecord& node = model->payload->nodes[morph.modelNode->value()];
            return node.kind == ModelNodeKind::Geometry && node.mesh && *node.mesh == morph.mesh;
        }

        std::optional<FrameId> mNextFrameId = InitialFrameId;
        HistoryEpoch mHistoryEpoch = InitialHistoryEpoch;
        std::optional<CameraState> mPreviousCamera;
        WorldEpoch mPreviousWorldEpoch;
        Extent2D mPreviousRenderExtent;
        Extent2D mPreviousOutputExtent;
        std::vector<DynamicTransformState> mPreviousDynamicTransforms;
        std::vector<SkeletonPoseState> mPreviousSkeletonPoses;
        std::vector<MorphWeightState> mPreviousMorphWeights;
    };
}

#endif
