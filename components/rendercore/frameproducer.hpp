#ifndef OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_FRAMEPRODUCER_H

#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace RenderCore
{
    struct DynamicTransformInput
    {
        InstanceHandle instance;
        WorldTransform transform;
        float opacity = 1.0f;
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
        struct DerivedShadowViews
        {
            bool enabled = false;
            std::uint32_t cascadeCount = 1;
            Extent2D extent{ 2048, 2048 };
            float maximumDistance = 1.0f;
        };

        struct WaterViews
        {
            bool enabled = false;
            bool reflection = true;
            bool refraction = true;
            Extent2D extent{ 512, 512 };
            float reflectionLodScale = 0.5f;
            float refractionLodScale = 0.5f;
        };

        // Explicit, reusable offscreen work for maps, previews, and diagnostic
        // views. The producer owns stable handles; callers own camera/policy.
        struct AuxiliaryView
        {
            ViewKind kind = ViewKind::Preview;
            CameraState camera;
            Extent2D extent{ 512, 512 };
            RenderTargetFormat colorFormat = RenderTargetFormat::Rgba8Srgb;
            std::optional<RenderTargetFormat> depthFormat = RenderTargetFormat::Depth32Float;
            float lodScale = 1.0f;
            std::uint64_t semanticIncludeMask = ~std::uint64_t{ 0 };
            std::uint64_t semanticExcludeMask = 0;
            std::optional<WorldClipPlane> clipPlane;
            bool temporal = false;
            bool transient = false;
            bool sampledByMain = false;

            // A caller-owned logical slot keeps a persistent auxiliary surface
            // on the same neutral view/target/pass handles even when other
            // requests are added or removed. Unspecified requests retain the
            // original positional behavior for one-shot preview/debug callers.
            std::optional<std::uint32_t> stableSlot;
        };

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
        DerivedShadowViews shadowViews;
        WaterViews waterViews;
        std::vector<AuxiliaryView> auxiliaryViews;
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

            const bool dimensionsMatch
                = mPreviousRenderExtent == input.renderExtent && mPreviousOutputExtent == input.outputExtent;
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
            view.identity = ViewHandle::fromParts(0, 1);
            view.current = input.camera;
            view.previous = continuous ? *mPreviousCamera : input.camera;
            view.extent = input.renderExtent;
            view.outputTarget = RenderTargetHandle::fromParts(0, 1);
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
            desc.renderTargets.push_back(RenderTargetDesc{
                .identity = view.outputTarget,
                .kind = RenderTargetKind::Swapchain,
                .extent = input.outputExtent,
                .colorFormat = RenderTargetFormat::SurfaceColor,
                .depthFormat = RenderTargetFormat::Depth32Float,
                .sampleCount = 1,
                .historyEpoch = candidateHistoryEpoch,
                .historyValid = continuous,
                .transient = false,
            });
            desc.views.push_back(std::move(view));

            std::vector<RenderTargetHandle> mainInputs;
            std::vector<RenderPassHandle> mainDependencies;
            if (input.waterViews.enabled && input.environment.waterEnabled)
            {
                if ((!input.waterViews.reflection && !input.waterViews.refraction)
                    || !input.waterViews.extent.valid()
                    || std::abs(input.environment.waterHeight)
                        > static_cast<double>(std::numeric_limits<float>::max()) * 0.5)
                    return std::nullopt;
                const auto addWaterView = [&](ViewKind kind, std::uint32_t slot, float auxiliaryLodScale) {
                    const RenderTargetHandle target = RenderTargetHandle::fromParts(slot, 1);
                    const ViewHandle identity = ViewHandle::fromParts(slot, 1);
                    const RenderPassHandle pass = RenderPassHandle::fromParts(slot, 1);
                    CameraState camera = input.camera;
                    WorldClipPlane clipPlane;
                    if (kind == ViewKind::Reflection)
                    {
                        const glm::mat4 sourceWorld = glm::inverse(input.camera.view);
                        glm::vec3 position(sourceWorld[3]);
                        glm::vec3 forward = -glm::normalize(glm::vec3(sourceWorld[2]));
                        glm::vec3 up = glm::normalize(glm::vec3(sourceWorld[1]));
                        position.z = static_cast<float>(2.0 * input.environment.waterHeight) - position.z;
                        forward.z = -forward.z;
                        up.z = -up.z;
                        // Rebuilding a right-handed camera from the reflected
                        // forward/up basis avoids the negative determinant of
                        // a raw reflection matrix and preserves face culling.
                        camera.view = glm::lookAtRH(position, position + forward, up);
                        const glm::mat4 cameraWorld = glm::inverse(camera.view);
                        camera.worldPosition = glm::dvec3(position);
                        camera.worldOrientation = glm::normalize(glm::quat_cast(glm::mat3(cameraWorld)));
                        clipPlane = { glm::vec3(0.0f, 0.0f, 1.0f), -input.environment.waterHeight };
                    }
                    else
                        clipPlane = { glm::vec3(0.0f, 0.0f, -1.0f), input.environment.waterHeight };

                    desc.renderTargets.push_back(RenderTargetDesc{
                        .identity = target,
                        .kind = RenderTargetKind::Offscreen,
                        .extent = input.waterViews.extent,
                        .colorFormat = RenderTargetFormat::Rgba16Float,
                        .depthFormat = RenderTargetFormat::Depth32Float,
                        .sampleCount = 1,
                        .historyEpoch = candidateHistoryEpoch,
                        .historyValid = continuous,
                        .transient = false,
                    });
                    desc.views.push_back(FrameView{
                        .identity = identity,
                        .viewIndex = slot,
                        .kind = kind,
                        .outputTarget = target,
                        .current = camera,
                        .previous = camera,
                        .extent = input.waterViews.extent,
                        .lodScale = auxiliaryLodScale,
                        .semanticIncludeMask = semanticFlag(kind == ViewKind::Reflection
                                ? InstanceSemanticFlag::ReflectionEligible
                                : InstanceSemanticFlag::RefractionEligible),
                        .semanticExcludeMask = 0,
                        .clipPlane = clipPlane,
                        .historyEpoch = candidateHistoryEpoch,
                        .temporal = false,
                        .historyValid = false,
                    });
                    desc.renderPasses.push_back(RenderPassDesc{
                        .identity = pass,
                        .view = identity,
                        .output = target,
                        .colorLoad = RenderPassLoad::Clear,
                        .depthLoad = RenderPassLoad::Clear,
                        .colorStore = RenderPassStore::Store,
                        .depthStore = RenderPassStore::Store,
                        .present = false,
                    });
                    mainInputs.push_back(target);
                    mainDependencies.push_back(pass);
                };
                if (input.waterViews.reflection)
                    addWaterView(ViewKind::Reflection, 2, input.waterViews.reflectionLodScale);
                if (input.waterViews.refraction)
                    addWaterView(ViewKind::Refraction, 3, input.waterViews.refractionLodScale);
            }

            std::vector<std::uint32_t> auxiliarySlots;
            auxiliarySlots.reserve(input.auxiliaryViews.size());
            for (std::size_t i = 0; i < input.auxiliaryViews.size(); ++i)
            {
                const SingleViewFrameInput::AuxiliaryView& request = input.auxiliaryViews[i];
                if (request.kind != ViewKind::Map && request.kind != ViewKind::Preview
                    && request.kind != ViewKind::Debug)
                    return std::nullopt;
                const std::uint64_t logicalSlot
                    = request.stableSlot ? static_cast<std::uint64_t>(*request.stableSlot)
                                         : static_cast<std::uint64_t>(i);
                if (logicalSlot > std::numeric_limits<std::uint32_t>::max() - 4ull)
                    return std::nullopt;
                const std::uint32_t slot = static_cast<std::uint32_t>(logicalSlot) + 4;
                if (std::find(auxiliarySlots.begin(), auxiliarySlots.end(), slot) != auxiliarySlots.end())
                    return std::nullopt;
                auxiliarySlots.push_back(slot);
                const RenderTargetHandle target = RenderTargetHandle::fromParts(slot, 1);
                const ViewHandle identity = ViewHandle::fromParts(slot, 1);
                const RenderPassHandle pass = RenderPassHandle::fromParts(slot, 1);
                desc.renderTargets.push_back(RenderTargetDesc{
                    .identity = target,
                    .kind = RenderTargetKind::Offscreen,
                    .extent = request.extent,
                    .colorFormat = request.colorFormat,
                    .depthFormat = request.depthFormat,
                    .sampleCount = 1,
                    .historyEpoch = candidateHistoryEpoch,
                    .historyValid = false,
                    .transient = request.transient,
                });
                desc.views.push_back(FrameView{
                    .identity = identity,
                    .viewIndex = slot,
                    .kind = request.kind,
                    .outputTarget = target,
                    .current = request.camera,
                    .previous = request.camera,
                    .extent = request.extent,
                    .lodScale = request.lodScale,
                    .semanticIncludeMask = request.semanticIncludeMask,
                    .semanticExcludeMask = request.semanticExcludeMask,
                    .clipPlane = request.clipPlane,
                    .historyEpoch = candidateHistoryEpoch,
                    .temporal = request.temporal,
                    .historyValid = false,
                });
                desc.renderPasses.push_back(RenderPassDesc{
                    .identity = pass,
                    .view = identity,
                    .output = target,
                    .colorLoad = RenderPassLoad::Clear,
                    .depthLoad = RenderPassLoad::Clear,
                    .colorStore = RenderPassStore::Store,
                    .depthStore = RenderPassStore::Store,
                    .present = false,
                });
                if (request.sampledByMain)
                {
                    mainInputs.push_back(target);
                    mainDependencies.push_back(pass);
                }
            }

            desc.renderPasses.push_back(RenderPassDesc{
                .identity = RenderPassHandle::fromParts(0, 1),
                .view = desc.views.front().identity,
                .output = desc.views.front().outputTarget,
                .inputs = std::move(mainInputs),
                .dependencies = std::move(mainDependencies),
                .present = true,
            });
            if (input.shadowViews.enabled && input.environment.shadowsEnabled)
            {
                desc.derivedViewFamilies.push_back(DerivedViewFamilyDesc{
                    .identity = ViewHandle::fromParts(1, 1),
                    .sourceView = desc.views.front().identity,
                    .kind = ViewKind::Shadow,
                    .extent = input.shadowViews.extent,
                    .viewCount = input.shadowViews.cascadeCount,
                    .maximumDistance = input.shadowViews.maximumDistance,
                    .semanticIncludeMask = semanticFlag(InstanceSemanticFlag::ShadowCaster),
                    .semanticExcludeMask = 0,
                    .transient = false,
                });
            }

            for (const DynamicTransformInput& inputTransform : input.dynamicTransforms)
            {
                const InstanceRecord* instance = world.get(inputTransform.instance);
                if (!instance)
                    return std::nullopt;
                const auto previous = std::find_if(mPreviousDynamicTransforms.begin(), mPreviousDynamicTransforms.end(),
                    [&](const DynamicTransformState& value) { return value.instance == inputTransform.instance; });
                const bool transformContinuous = continuous && previous != mPreviousDynamicTransforms.end()
                    && previous->instanceRevision == instance->revision;
                DynamicTransformState transform;
                transform.instance = inputTransform.instance;
                transform.instanceRevision = instance->revision;
                transform.current = inputTransform.transform;
                transform.previous = transformContinuous ? previous->current : inputTransform.transform;
                transform.historyValid = transformContinuous;
                transform.opacity = inputTransform.opacity;
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

                const auto previous = std::find_if(
                    mPreviousMorphWeights.begin(), mPreviousMorphWeights.end(), [&](const MorphWeightState& value) {
                        return value.instance == inputMorph.instance && value.modelNode == inputMorph.modelNode;
                    });
                const bool morphContinuous = continuous && previous != mPreviousMorphWeights.end()
                    && previous->instanceRevision == instance->revision && previous->mesh == inputMorph.mesh
                    && previous->meshRevision == mesh->revision
                    && previous->current.size() == inputMorph.weights.size();

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
            if (!mNextFrameId || !frame.valid() || frame.frameId() != *mNextFrameId || frame.views().empty()
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
