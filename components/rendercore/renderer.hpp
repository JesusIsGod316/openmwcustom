#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERER_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERER_H

#include "backendselection.hpp"
#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <cstdint>

namespace RenderCore
{
    // Renderer entry coherence gate. Backends must not consume frame-local state
    // against a different logical world revision, or dereference dynamic handles
    // that were retired/replaced after the frame snapshot was assembled.
    [[nodiscard]] inline bool frameCompatibleWithWorld(
        const RenderWorld& world, const FrameRenderState& frame) noexcept
    {
        if (!frame.valid() || frame.worldEpoch() != world.epoch()
            || frame.renderWorldRevision() != world.revision())
            return false;

        for (const DynamicTransformState& transform : frame.dynamicTransforms())
        {
            const InstanceRecord* instance = world.get(transform.instance);
            if (!instance || instance->revision != transform.instanceRevision)
                return false;
        }

        for (const SkeletonPoseState& pose : frame.skeletonPoses())
        {
            const InstanceRecord* instance = world.get(pose.instance);
            const SkeletonRecord* skeleton = world.get(pose.skeleton);
            if (!instance || instance->revision != pose.instanceRevision || instance->skeleton != pose.skeleton
                || !skeleton || skeleton->revision != pose.skeletonRevision || !skeleton->payload
                || skeleton->payload->bones.size() != pose.current.size())
                return false;
        }

        for (const MorphWeightState& morph : frame.morphWeights())
        {
            const InstanceRecord* instance = world.get(morph.instance);
            const MeshRecord* mesh = world.get(morph.mesh);
            if (!instance || instance->revision != morph.instanceRevision || !mesh
                || mesh->revision != morph.meshRevision || !mesh->morphs
                || mesh->morphs->targets.size() != morph.current.size())
                return false;
            if (morph.modelNode)
            {
                const ModelRecord* model = instance->model ? world.get(*instance->model) : nullptr;
                if (!model || !model->payload || morph.modelNode->value() >= model->payload->nodes.size())
                    return false;
                const ModelNodeRecord& node = model->payload->nodes[morph.modelNode->value()];
                if (node.kind != ModelNodeKind::Geometry || node.mesh != morph.mesh)
                    return false;
            }
            else if (instance->model || instance->mesh != morph.mesh)
                return false;
        }

        for (const DynamicMaterialState& state : frame.dynamicMaterials())
        {
            const MaterialRecord* material = world.get(state.material);
            if (!material)
                return false;
            for (const DynamicTextureTransformState& transform : state.textureTransforms)
            {
                if (transform.bindingIndex >= material->textures.size())
                    return false;
            }
        }
        return true;
    }

    enum class RenderFrameResult : std::uint8_t
    {
        Presented,
        Skipped,
        Failed,
    };

    // Deliberately narrow semantic backend seam. The game publishes logical world
    // and immutable frame state above this boundary; backend objects never cross it.
    class SemanticRenderer
    {
    public:
        virtual ~SemanticRenderer() = default;
        [[nodiscard]] virtual RenderBackendKind backendKind() const noexcept = 0;
        virtual RenderFrameResult renderFrame(const RenderWorld& world, const FrameRenderState& frame) = 0;
        virtual void waitIdle() = 0;
    };
}

#endif
