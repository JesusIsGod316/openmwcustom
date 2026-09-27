#ifndef OPENMW_RENDER_VSG_DYNAMICACTORPREPARATION_H
#define OPENMW_RENDER_VSG_DYNAMICACTORPREPARATION_H

#include "dynamicactorplan.hpp"
#include <components/rendercore/deformation.hpp>
#include <components/rendercore/boundedparallelfor.hpp>
#include <unordered_map>
#include <span>

namespace RenderVsg
{
    struct PreparedDynamicActor
    {
        const RenderCore::DynamicTransformState* transform = nullptr;
        std::optional<StaticAssetPlan> asset;
        std::unordered_map<std::uint32_t, RenderCore::MeshPayload> deformed;
        std::string diagnostic;
        bool streamsOnly = false;
        const StaticAssetPlan* retainedAsset = nullptr;
        std::vector<glm::mat4> drawTransforms;
        mutable std::unordered_map<std::uint32_t, RenderCore::MeshPayload> realizationPayloads;

        const StaticAssetPlan* assetPlan() const noexcept { return retainedAsset ? retainedAsset : asset ? &*asset : nullptr; }

        // The warm resident updater needs only positions/normals. Cold creation
        // still needs the complete authored mesh (indices, UVs, colors, surfaces).
        // Materialize it lazily, once per affected node, not every animated frame.
        const RenderCore::MeshPayload* realizationPayload(const RenderCore::RenderWorld& world,
            RenderCore::MeshHandle meshHandle, RenderCore::ModelNodeIndex node) const
        {
            const auto found = deformed.find(node.value());
            if (found == deformed.end()) return nullptr;
            if (!streamsOnly) return &found->second;
            if (const auto cached = realizationPayloads.find(node.value()); cached != realizationPayloads.end())
                return &cached->second;
            const auto* mesh = world.get(meshHandle);
            if (!mesh || !mesh->payload) return nullptr;
            auto payload = *mesh->payload;
            payload.positions = found->second.positions;
            payload.normals = found->second.normals;
            payload.tangents = found->second.tangents;
            payload.bitangents = found->second.bitangents;
            return &realizationPayloads.emplace(node.value(), std::move(payload)).first->second;
        }
    };

    // Only immutable, published neutral inputs are borrowed. Jobs own separate
    // output slots; all join before any VSG object, descriptor or array changes.
    // No OSG/Lua/gameplay callback, GPU allocation or shared cache runs here.
    inline std::vector<PreparedDynamicActor> prepareDynamicActors(const RenderCore::RenderWorld& world,
        const RenderCore::FrameRenderState& frame, std::span<const DynamicActorPlan> actors,
        RenderCore::BoundedParallelFor* workers = nullptr, bool streamsOnly = false)
    {
        std::vector<PreparedDynamicActor> result(actors.size());
        const auto prepare = [&](std::size_t index) {
            const auto& actor = actors[index];
            auto& output = result[index];
            output.streamsOnly = streamsOnly;
            const auto transform = std::find_if(frame.dynamicTransforms().begin(), frame.dynamicTransforms().end(),
                [&](const auto& value) { return value.instance == actor.instance; });
            if (transform == frame.dynamicTransforms().end())
            { output.diagnostic = "dynamic actor frame is missing its current world transform"; return; }
            output.transform = &*transform;
            std::vector<glm::mat4> nodes;
            output.asset = evaluateDynamicActorAssetPlan(world, frame, actor, &nodes);
            if (!output.asset)
            { output.diagnostic = "dynamic actor draw transforms rejected the evaluated skeleton pose"; return; }
            for (const auto& draw : output.asset->draws)
            {
                const auto* mesh = world.get(draw.mesh);
                if (!mesh || (!mesh->skinned && !mesh->morphed) || output.deformed.contains(draw.node.value())) continue;
                auto deformation = RenderCore::deformMesh(world, frame, actor.instance, draw.mesh, draw.node, false, &nodes);
                if (!deformation.ready() || !mesh->payload)
                { output.diagnostic = "dynamic actor CPU deformation rejected its evaluated pose or morph state"; return; }
                // No full payload validation/realization consumes this sparse
                // container. Only the stream updater may borrow it directly.
                RenderCore::MeshPayload payload = streamsOnly ? RenderCore::MeshPayload{} : *mesh->payload;
                payload.positions = std::move(deformation.positions);
                payload.normals = std::move(deformation.normals);
                payload.tangents = std::move(deformation.tangents);
                payload.bitangents = std::move(deformation.bitangents);
                output.deformed.emplace(draw.node.value(), std::move(payload));
            }
        };
        if (workers) workers->forEach(result.size(), prepare);
        else for (std::size_t i = 0; i < result.size(); ++i) prepare(i);
        return result;
    }

    inline std::vector<PreparedDynamicActor> prepareDynamicActors(const RenderCore::RenderWorld& world,
        const RenderCore::FrameRenderState& frame, const DynamicActorWorldPlan& plan,
        RenderCore::BoundedParallelFor* workers = nullptr, bool streamsOnly = false)
    {
        return prepareDynamicActors(world, frame, std::span<const DynamicActorPlan>(plan.actors), workers, streamsOnly);
    }

    inline std::vector<PreparedDynamicActor> preparePersistentActors(const RenderCore::RenderWorld& world,
        const RenderCore::FrameRenderState& frame, std::span<const std::shared_ptr<const DynamicActorPlan>> actors,
        RenderCore::BoundedParallelFor* workers = nullptr)
    {
        std::vector<PreparedDynamicActor> result(actors.size());
        const auto prepare = [&](std::size_t index) {
            const auto& actor = *actors[index];
            auto& output = result[index];
            // An uncovered asset retains its existing evaluator. No global
            // switch to the fallback and no silently omitted body/equipment.
            if (!actor.program)
            {
                auto fallback = prepareDynamicActors(world, frame, std::span(&actor, 1), nullptr, true);
                output = std::move(fallback.front());
                return;
            }
            output.streamsOnly = true;
            output.retainedAsset = &actor.asset;
            const auto transform = std::find_if(frame.dynamicTransforms().begin(), frame.dynamicTransforms().end(),
                [&](const auto& value) { return value.instance == actor.instance; });
            const auto* pose = RenderCore::deformation_detail::findPose(frame, actor.instance);
            RenderCore::ActorProgram::Pose evaluated;
            if (transform == frame.dynamicTransforms().end() || !pose || !actor.program->evaluatePose(*pose, evaluated))
            { output.diagnostic = "persistent actor has missing/invalid frame pose or placement"; return; }
            output.transform = &*transform;
            output.drawTransforms.reserve(actor.asset.draws.size());
            for (const auto& draw : actor.asset.draws)
            {
                const auto* mesh = world.get(draw.mesh);
                if (!mesh || draw.node.value() >= evaluated.nodes.size())
                { output.diagnostic = "persistent actor has a stale draw binding"; return; }
                const bool skeletonSpace = mesh->skinned && (!mesh->skin || !mesh->skin->geometryBindTransform);
                output.drawTransforms.push_back(skeletonSpace ? glm::mat4(1) : evaluated.nodes[draw.node.value()]);
            }
            for (const auto& binding : actor.program->meshes())
            {
                // Hidden/collision-only meshes have bindings but no draw; do
                // not require morph input or deform streams that cannot render.
                if (std::none_of(actor.asset.draws.begin(), actor.asset.draws.end(),
                    [&](const auto& draw) { return draw.node == binding.node; })) continue;
                const auto* morph = binding.morphs
                    ? RenderCore::deformation_detail::findMorph(frame, actor.instance, binding.mesh, binding.node) : nullptr;
                auto& streams = output.deformed[binding.node.value()];
                if (!actor.program->deform(binding, evaluated, morph, streams))
                { output.diagnostic = "persistent actor rejected skin cancellation or morph weights"; return; }
            }
        };
        if (workers) workers->forEach(result.size(), prepare);
        else for (std::size_t i = 0; i < result.size(); ++i) prepare(i);
        return result;
    }
}
#endif
