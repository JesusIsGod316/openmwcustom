#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_DYNAMICACTORPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_DYNAMICACTORPLAN_H

#include "staticassetplan.hpp"
#include "staticworldplan.hpp"

#include <components/rendercore/framerenderstate.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

namespace RenderVsg
{
    struct DynamicActorPlan
    {
        RenderCore::InstanceHandle instance;
        RenderCore::ResourceRevision instanceRevision;
        RenderCore::ModelHandle model;
        RenderCore::ResourceRevision modelRevision;
        RenderCore::SkeletonHandle skeleton;
        RenderCore::ResourceRevision skeletonRevision;
        std::optional<RenderCore::ChunkHandle> chunk;
        RenderCore::WorldTransform placement;
        std::uint64_t semanticFlags = 0;
        bool lightingEnabled = true;
        StaticPlanOptions options;
        StaticAssetPlan asset;
        std::vector<StaticResourceDependency<RenderCore::MeshHandle>> meshes;
        std::vector<StaticResourceDependency<RenderCore::MaterialHandle>> materials;
        std::vector<StaticResourceDependency<RenderCore::TextureHandle>> textures;
    };

    struct DynamicActorWorldPlan
    {
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        std::vector<DynamicActorPlan> actors;
        std::uint32_t invalidActors = 0;

        [[nodiscard]] bool valid() const noexcept
        {
            return worldEpoch.valid() && worldRevision.valid() && invalidActors == 0;
        }
    };

    [[nodiscard]] inline std::optional<DynamicActorPlan> buildDynamicActorPlan(
        const RenderCore::RenderWorld& world, RenderCore::InstanceHandle handle, StaticPlanOptions options = {})
    {
        using namespace RenderCore;
        const InstanceRecord* instance = world.get(handle);
        if (!instance || !instance->revision.valid() || !instance->model || !instance->skeleton
            || instance->mesh.valid() || instance->attachment)
            return std::nullopt;
        const ModelRecord* model = world.get(*instance->model);
        const SkeletonRecord* skeleton = world.get(*instance->skeleton);
        if (!model || !model->revision.valid() || !skeleton || !skeleton->revision.valid()
            || !skeleton->payload || !validSkeletonPayload(*skeleton->payload)
            || !validModelDynamicRequirements(model->dynamicRequirements)
            || model->dynamicRequirements != 0)
            return std::nullopt;

        options.includeDeformableMeshes = true;
        std::optional<StaticAssetPlan> asset = buildStaticAssetPlan(world, *instance->model, options);
        if (!asset || asset->dynamicMeshesDeferred != 0)
            return std::nullopt;

        const auto equalFolded = [](std::string_view left, std::string_view right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        };
        const auto skeletonContains = [&](std::string_view name) {
            return std::any_of(skeleton->payload->bones.begin(), skeleton->payload->bones.end(),
                [&](const BoneRecord& bone) { return equalFolded(bone.name, name); });
        };
        for (const ModelNodeRecord& node : model->payload->nodes)
        {
            const std::uint32_t unsupported = modelControllerFlag(ModelControllerFlag::Visibility)
                | modelControllerFlag(ModelControllerFlag::Unsupported);
            if ((node.controllerFlags & unsupported) != 0)
                return std::nullopt;
            if ((node.controllerFlags & modelControllerFlag(ModelControllerFlag::Transform)) != 0
                && !skeletonContains(node.name))
                return std::nullopt;
            if ((node.controllerFlags & modelControllerFlag(ModelControllerFlag::Morph)) != 0)
            {
                const MeshRecord* mesh = node.mesh ? world.get(*node.mesh) : nullptr;
                if (!mesh || !mesh->morphed)
                    return std::nullopt;
            }
        }

        DynamicActorPlan result{
            .instance = handle,
            .instanceRevision = instance->revision,
            .model = *instance->model,
            .modelRevision = model->revision,
            .skeleton = *instance->skeleton,
            .skeletonRevision = skeleton->revision,
            .chunk = instance->chunk,
            .placement = instance->transform,
            .semanticFlags = instance->semanticFlags,
            .lightingEnabled = instance->lightingEnabled,
            .options = options,
            .asset = std::move(*asset),
            .meshes = {},
            .materials = {},
            .textures = {},
        };

        const auto addUnique = []<class Handle>(std::vector<StaticResourceDependency<Handle>>& dependencies,
                                   Handle dependency, ResourceRevision revision) {
            const StaticResourceDependency<Handle> candidate{ dependency, revision };
            if (std::find(dependencies.begin(), dependencies.end(), candidate) == dependencies.end())
                dependencies.push_back(candidate);
        };
        bool hasDeformableMesh = false;
        for (const StaticDrawPlan& draw : result.asset.draws)
        {
            const MeshRecord* mesh = world.get(draw.mesh);
            const MaterialRecord* material = world.get(draw.material);
            if (!mesh || !material)
                return std::nullopt;
            hasDeformableMesh = hasDeformableMesh || mesh->skinned || mesh->morphed;
            addUnique(result.meshes, draw.mesh, mesh->revision);
            addUnique(result.materials, draw.material, material->revision);
            for (const TextureRealizationKey& texture : draw.textures)
            {
                const TextureRecord* record = world.get(texture.view.texture);
                if (!record)
                    return std::nullopt;
                addUnique(result.textures, texture.view.texture, record->revision);
            }
        }
        if (!hasDeformableMesh)
            return std::nullopt;
        return result;
    }

    [[nodiscard]] inline bool dynamicActorPlanCurrent(
        const RenderCore::RenderWorld& world, const DynamicActorPlan& plan) noexcept
    {
        const RenderCore::InstanceRecord* instance = world.get(plan.instance);
        const RenderCore::ModelRecord* model = world.get(plan.model);
        const RenderCore::SkeletonRecord* skeleton = world.get(plan.skeleton);
        if (!instance || !model || !skeleton || instance->revision != plan.instanceRevision
            || instance->model != plan.model || instance->skeleton != plan.skeleton
            || model->revision != plan.modelRevision || skeleton->revision != plan.skeletonRevision)
            return false;
        for (const auto& dependency : plan.meshes)
        {
            const RenderCore::MeshRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return false;
        }
        for (const auto& dependency : plan.materials)
        {
            const RenderCore::MaterialRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return false;
        }
        for (const auto& dependency : plan.textures)
        {
            const RenderCore::TextureRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return false;
        }
        return true;
    }

    [[nodiscard]] inline DynamicActorWorldPlan buildDynamicActorWorldPlan(
        const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
    {
        DynamicActorWorldPlan result;
        result.worldEpoch = world.epoch();
        result.worldRevision = world.revision();
        world.forEachInstance([&](RenderCore::InstanceHandle handle, const RenderCore::InstanceRecord& instance) {
            if (!instance.skeleton)
                return;
            std::optional<DynamicActorPlan> actor = buildDynamicActorPlan(world, handle, options);
            if (actor)
                result.actors.push_back(std::move(*actor));
            else
                ++result.invalidActors;
        });
        return result;
    }

    [[nodiscard]] inline std::optional<StaticAssetPlan> evaluateDynamicActorAssetPlan(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame,
        const DynamicActorPlan& actor)
    {
        using namespace RenderCore;
        const ModelRecord* model = world.get(actor.model);
        const SkeletonRecord* skeleton = world.get(actor.skeleton);
        const auto pose = std::find_if(frame.skeletonPoses().begin(), frame.skeletonPoses().end(),
            [&](const SkeletonPoseState& value) { return value.instance == actor.instance; });
        if (!model || !model->payload || !skeleton || !skeleton->payload
            || pose == frame.skeletonPoses().end() || pose->skeleton != actor.skeleton
            || pose->current.size() != skeleton->payload->bones.size())
            return std::nullopt;

        const auto equalFolded = [](std::string_view left, std::string_view right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        };
        std::vector<glm::mat4> globalBones(pose->current.size());
        for (std::size_t i = 0; i < pose->current.size(); ++i)
        {
            const std::int32_t parent = skeleton->payload->bones[i].parent;
            globalBones[i] = parent < 0 ? pose->current[i]
                                        : globalBones[static_cast<std::size_t>(parent)] * pose->current[i];
        }
        std::vector<glm::mat4> nodeWorld(model->payload->nodes.size());
        for (std::size_t i = 0; i < model->payload->nodes.size(); ++i)
        {
            const ModelNodeRecord& node = model->payload->nodes[i];
            const auto bone = std::find_if(skeleton->payload->bones.begin(), skeleton->payload->bones.end(),
                [&](const BoneRecord& value) { return equalFolded(value.name, node.name); });
            if (bone != skeleton->payload->bones.end())
            {
                nodeWorld[i] = globalBones[static_cast<std::size_t>(bone - skeleton->payload->bones.begin())];
                continue;
            }
            nodeWorld[i] = node.parent.valid() ? nodeWorld[node.parent.value()] * node.localTransform
                                               : node.localTransform;
        }

        StaticAssetPlan result = actor.asset;
        for (StaticDrawPlan& draw : result.draws)
        {
            const MeshRecord* mesh = world.get(draw.mesh);
            if (!mesh || draw.node.value() >= nodeWorld.size())
                return std::nullopt;
            // Skinning produces skeleton-space vertices. Rigid and morph-only
            // attachments retain their evaluated model-node placement.
            draw.worldTransform = mesh->skinned ? glm::mat4(1.0f) : nodeWorld[draw.node.value()];
        }
        return result;
    }
}

#endif
