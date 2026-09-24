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
    };

    // Only immutable, published neutral inputs are borrowed. Jobs own separate
    // output slots; all join before any VSG object, descriptor or array changes.
    // No OSG/Lua/gameplay callback, GPU allocation or shared cache runs here.
    inline std::vector<PreparedDynamicActor> prepareDynamicActors(const RenderCore::RenderWorld& world,
        const RenderCore::FrameRenderState& frame, std::span<const DynamicActorPlan> actors,
        RenderCore::BoundedParallelFor* workers = nullptr)
    {
        std::vector<PreparedDynamicActor> result(actors.size());
        const auto prepare = [&](std::size_t index) {
            const auto& actor = actors[index];
            auto& output = result[index];
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
                RenderCore::MeshPayload payload = *mesh->payload;
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
        RenderCore::BoundedParallelFor* workers = nullptr)
    {
        return prepareDynamicActors(world, frame, std::span<const DynamicActorPlan>(plan.actors), workers);
    }
}
#endif
