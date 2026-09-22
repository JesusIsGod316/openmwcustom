#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_DYNAMICACTORPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_DYNAMICACTORPLAN_H

#include "staticassetplan.hpp"
#include "staticworldplan.hpp"

#include <components/rendercore/framerenderstate.hpp>
#include <components/rendercore/posedmodel.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept
        {
            return worldEpoch.valid() && worldRevision.valid() && invalidActors == 0;
        }
    };

    [[nodiscard]] inline std::optional<DynamicActorPlan> buildDynamicActorPlan(
        const RenderCore::RenderWorld& world, RenderCore::InstanceHandle handle, StaticPlanOptions options = {},
        std::string* diagnostic = nullptr)
    {
        using namespace RenderCore;
        const auto fail = [&](std::string message) -> std::optional<DynamicActorPlan> {
            if (diagnostic)
                *diagnostic = std::move(message);
            return std::nullopt;
        };
        const InstanceRecord* instance = world.get(handle);
        if (!instance || !instance->revision.valid() || !instance->model || !instance->skeleton
            || instance->mesh.valid() || instance->attachment)
            return fail("invalid actor instance ownership");
        const ModelRecord* model = world.get(*instance->model);
        const SkeletonRecord* skeleton = world.get(*instance->skeleton);
        if (!model || !model->revision.valid() || !skeleton || !skeleton->revision.valid()
            || !skeleton->payload || !validSkeletonPayload(*skeleton->payload)
            || !validModelDynamicRequirements(model->dynamicRequirements)
            || model->dynamicRequirements != 0)
            return fail("invalid actor model/skeleton record: "
                + (model ? model->sourceIdentity : std::string("<missing-model>")) + " / "
                + (skeleton ? skeleton->sourceIdentity : std::string("<missing-skeleton>")));

        options.includeDeformableMeshes = true;
        std::string assetDiagnostic;
        std::optional<StaticAssetPlan> asset
            = buildStaticAssetPlan(world, *instance->model, options, &assetDiagnostic);
        if (!asset || asset->dynamicMeshesDeferred != 0)
            return fail("actor static-asset dependency planning failed for " + model->sourceIdentity
                + (assetDiagnostic.empty() ? std::string{} : ": " + assetDiagnostic));

        // A composed actor can contain rigid hair/equipment subgraphs with
        // model-local transform controllers. OpenMW attaches those subgraphs
        // below a skeleton bone; it does not require each local controller
        // target to be a bone in the external actor skeleton. During pose
        // evaluation, matching names consume the actor pose and non-matching
        // names retain their authored local transform.
        for (const ModelNodeRecord& node : model->payload->nodes)
        {
            const std::uint32_t unsupported = modelControllerFlag(ModelControllerFlag::Visibility)
                | modelControllerFlag(ModelControllerFlag::Unsupported);
            if ((node.controllerFlags & unsupported) != 0)
                return fail("actor node has unsupported visibility/controller semantics: " + node.name);
            if ((node.controllerFlags & modelControllerFlag(ModelControllerFlag::Morph)) != 0)
            {
                const MeshRecord* mesh = node.mesh ? world.get(*node.mesh) : nullptr;
                if (!mesh || !mesh->morphed)
                    return fail("actor morph controller has no morphed mesh: " + node.name
                        + " in " + model->sourceIdentity);
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
        // Hidden/disabled nodes can affect material ordering and validation too.
        // A cached plan must track them, not only its currently emitted draws.
        for (const ModelNodeRecord& node : model->payload->nodes)
        {
            if (node.mesh)
            {
                const MeshRecord* mesh = world.get(*node.mesh);
                if (!mesh) return fail("actor node has a stale mesh dependency");
                addUnique(result.meshes, *node.mesh, mesh->revision);
            }
            for (const MaterialHandle materialHandle : node.materials)
            {
                const MaterialRecord* material = world.get(materialHandle);
                if (!material) return fail("actor node has a stale material dependency");
                addUnique(result.materials, materialHandle, material->revision);
                for (const TextureBinding& binding : material->textures)
                {
                    const TextureRecord* record = world.get(binding.texture);
                    if (!record) return fail("actor node has a stale texture dependency");
                    addUnique(result.textures, binding.texture, record->revision);
                }
            }
        }
        // OpenMW actors can be assembled entirely from rigid geometry beneath
        // animated transform bones (for example vanilla-style creatures). Their
        // pose is applied by evaluateDynamicActorAssetPlan, not vertex skinning.
        // Keep empty/unsupported asset rejection distinct from that valid case.
        if (result.asset.draws.empty())
            return fail("actor model has no currently drawable mesh: " + model->sourceIdentity);
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
            std::string diagnostic;
            std::optional<DynamicActorPlan> actor = buildDynamicActorPlan(world, handle, options, &diagnostic);
            if (actor)
                result.actors.push_back(std::move(*actor));
            else
            {
                ++result.invalidActors;
                if (result.diagnostic.empty())
                    result.diagnostic = std::move(diagnostic);
            }
        });
        return result;
    }

    // Cache only immutable planning, never evaluated poses or mutable GPU data.
    // Entries own draw metadata, not mesh payload copies; removal/epoch changes
    // prune them. Invalid dependencies always return to the full validator.
    class DynamicActorPlanCache
    {
    public:
        std::size_t rebuilt = 0;
        std::size_t reused = 0;

        [[nodiscard]] DynamicActorWorldPlan prepare(const RenderCore::RenderWorld& world,
            StaticPlanOptions options = {}, bool rebuildControl = false)
        {
            if (mEpoch != world.epoch())
                mPlans.clear();
            mEpoch = world.epoch();
            options.includeDeformableMeshes = true;
            rebuilt = reused = 0;
            DynamicActorWorldPlan result;
            result.worldEpoch = world.epoch();
            result.worldRevision = world.revision();
            std::unordered_set<std::uint64_t> active;
            world.forEachInstance([&](RenderCore::InstanceHandle handle, const RenderCore::InstanceRecord& instance) {
                if (!instance.skeleton) return;
                const std::uint64_t key = (std::uint64_t(handle.generation()) << 32u) | handle.slot();
                active.insert(key);
                auto found = mPlans.find(key);
                if (!rebuildControl && found != mPlans.end() && found->second.options == options
                    && dynamicActorPlanCurrent(world, found->second))
                {
                    ++reused;
                    result.actors.push_back(found->second);
                    return;
                }
                ++rebuilt;
                std::string diagnostic;
                auto plan = buildDynamicActorPlan(world, handle, options, &diagnostic);
                if (plan)
                {
                    mPlans.insert_or_assign(key, *plan);
                    result.actors.push_back(std::move(*plan));
                }
                else
                {
                    mPlans.erase(key);
                    ++result.invalidActors;
                    if (result.diagnostic.empty()) result.diagnostic = std::move(diagnostic);
                }
            });
            std::erase_if(mPlans, [&](const auto& entry) { return !active.contains(entry.first); });
            return result;
        }

        [[nodiscard]] std::size_t size() const noexcept { return mPlans.size(); }

    private:
        RenderCore::WorldEpoch mEpoch;
        std::unordered_map<std::uint64_t, DynamicActorPlan> mPlans;
    };

    [[nodiscard]] inline std::optional<StaticAssetPlan> evaluateDynamicActorAssetPlan(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame,
        const DynamicActorPlan& actor, std::vector<glm::mat4>* evaluatedModelNodes = nullptr)
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

        std::vector<glm::mat4> globalBones(pose->current.size());
        for (std::size_t i = 0; i < pose->current.size(); ++i)
        {
            const std::int32_t parent = skeleton->payload->bones[i].parent;
            globalBones[i] = parent < 0 ? pose->current[i]
                                        : globalBones[static_cast<std::size_t>(parent)] * pose->current[i];
        }
        auto nodeWorld = posedModelTransforms(*model->payload, *skeleton->payload, globalBones);

        StaticAssetPlan result = actor.asset;
        for (StaticDrawPlan& draw : result.draws)
        {
            const MeshRecord* mesh = world.get(draw.mesh);
            if (!mesh || draw.node.value() >= nodeWorld.size())
                return std::nullopt;
            // NIF skinning retains geometry-local output just like RigGeometry.
            // Only generic skeleton-space producers omit node placement.
            const bool skeletonSpace = mesh->skinned && (!mesh->skin || !mesh->skin->geometryBindTransform);
            draw.worldTransform = skeletonSpace ? glm::mat4(1.0f) : nodeWorld[draw.node.value()];
        }
        if (evaluatedModelNodes) *evaluatedModelNodes = std::move(nodeWorld);
        return result;
    }
}

#endif
