#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETPLAN_H

#include <components/rendercore/lodselection.hpp>
#include <components/rendercore/realizationkeys.hpp>
#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/texturerealization.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RenderVsg
{
    struct StaticPlanOptions
    {
        float lodEyeDistance = 0.0f;
        bool showMarkers = false;
    };

    struct StaticDrawPlan
    {
        RenderCore::ModelNodeIndex node;
        RenderCore::MeshHandle mesh;
        RenderCore::MaterialHandle material;
        std::uint32_t surfaceIndex = 0;
        RenderCore::MeshSurface surface;
        glm::mat4 worldTransform{ 1.0f };
        std::optional<RenderCore::ModelBillboardMode> billboard;
        std::optional<RenderCore::ModelSortSemantic> sort;
        RenderCore::GraphicsPipelineKey pipeline;
        RenderCore::MaterialRealizationKey materialRealization;
        std::vector<RenderCore::TextureRealizationKey> textures;
        std::vector<RenderCore::SamplerRealizationKey> samplers;
    };

    struct StaticAssetPlan
    {
        std::vector<StaticDrawPlan> draws;
        std::uint32_t hiddenNodes = 0;
        std::uint32_t collisionOnlyNodes = 0;
        std::uint32_t markerNodes = 0;
        std::uint32_t switchSuppressedNodes = 0;
        std::uint32_t lodSuppressedNodes = 0;
        std::uint32_t dynamicMeshesDeferred = 0;

        [[nodiscard]] bool empty() const noexcept { return draws.empty(); }
    };

    namespace static_plan_detail
    {
        struct TraversalState
        {
            glm::mat4 world{ 1.0f };
            std::optional<RenderCore::ModelBillboardMode> billboard;
            std::optional<RenderCore::ModelSortSemantic> sort;
        };

        [[nodiscard]] inline bool hasFlag(const RenderCore::ModelNodeRecord& node, RenderCore::ModelNodeFlag flag)
        {
            return (node.flags & RenderCore::modelNodeFlag(flag)) != 0;
        }

        inline void countSubtree(const RenderCore::ModelPayload& payload,
            const std::vector<std::vector<RenderCore::ModelNodeIndex>>& children, RenderCore::ModelNodeIndex root,
            std::uint32_t& counter)
        {
            if (!root.valid() || root.value() >= payload.nodes.size())
                return;
            ++counter;
            for (const RenderCore::ModelNodeIndex child : children[root.value()])
                countSubtree(payload, children, child, counter);
        }
    }

    // Build a backend-ready immutable draw plan from published neutral resources.
    // This is deliberately VSG-type-free: the subsequent backend realization may
    // allocate VSG objects, but hierarchy selection, resource identity and cache
    // key derivation remain testable without a Vulkan device or a source NIF.
    [[nodiscard]] inline std::optional<StaticAssetPlan> buildStaticAssetPlan(
        const RenderCore::RenderWorld& world, RenderCore::ModelHandle modelHandle, StaticPlanOptions options = {})
    {
        using namespace RenderCore;
        const ModelRecord* model = world.get(modelHandle);
        if (!model || !model->payload || !validModelPayloadStructure(*model->payload)
            || !semantic_detail::finite(options.lodEyeDistance))
            return std::nullopt;

        const ModelPayload& payload = *model->payload;
        std::vector<std::vector<ModelNodeIndex>> children(payload.nodes.size());
        for (std::size_t i = 0; i < payload.nodes.size(); ++i)
        {
            if (payload.nodes[i].parent.valid())
                children[payload.nodes[i].parent.value()].emplace_back(static_cast<std::uint32_t>(i));
        }

        StaticAssetPlan plan;
        const auto visit = [&](const auto& self, ModelNodeIndex index, static_plan_detail::TraversalState inherited) -> bool {
            if (!index.valid() || index.value() >= payload.nodes.size())
                return false;
            const ModelNodeRecord& node = payload.nodes[index.value()];

            static_plan_detail::TraversalState state = inherited;
            state.world = inherited.world * node.localTransform;
            if (node.billboard)
                state.billboard = node.billboard;
            if (node.sort)
                state.sort = node.sort;

            if (static_plan_detail::hasFlag(node, ModelNodeFlag::Hidden))
            {
                static_plan_detail::countSubtree(payload, children, index, plan.hiddenNodes);
                return true;
            }
            if (static_plan_detail::hasFlag(node, ModelNodeFlag::CollisionOnly))
            {
                static_plan_detail::countSubtree(payload, children, index, plan.collisionOnlyNodes);
                return true;
            }
            if (static_plan_detail::hasFlag(node, ModelNodeFlag::Marker) && !options.showMarkers)
            {
                static_plan_detail::countSubtree(payload, children, index, plan.markerNodes);
                return true;
            }

            if (node.kind == ModelNodeKind::Geometry)
            {
                if (!node.mesh)
                    return false;
                const MeshRecord* mesh = world.get(*node.mesh);
                if (!mesh || !mesh->payload || !validMeshPayload(*mesh->payload)
                    || mesh->surfaceCount != mesh->payload->surfaces.size())
                    return false;

                if (mesh->skinned || mesh->morphed)
                {
                    ++plan.dynamicMeshesDeferred;
                    return true;
                }

                for (std::size_t surfaceIndex = 0; surfaceIndex < mesh->payload->surfaces.size(); ++surfaceIndex)
                {
                    const MeshSurface& surface = mesh->payload->surfaces[surfaceIndex];
                    if (surface.materialSlot >= node.materials.size())
                        return false;
                    const MaterialHandle materialHandle = node.materials[surface.materialSlot];
                    const MaterialRecord* material = world.get(materialHandle);
                    if (!material)
                        return false;

                    StaticDrawPlan draw;
                    draw.node = index;
                    draw.mesh = *node.mesh;
                    draw.material = materialHandle;
                    draw.surfaceIndex = static_cast<std::uint32_t>(surfaceIndex);
                    draw.surface = surface;
                    draw.worldTransform = state.world;
                    draw.billboard = state.billboard;
                    draw.sort = state.sort;
                    draw.pipeline = makeGraphicsPipelineKey(*mesh->payload, surface.topology, *material);
                    draw.materialRealization = makeMaterialRealizationKey(*material);
                    draw.textures.reserve(material->textures.size());
                    draw.samplers.reserve(material->textures.size());
                    for (const TextureBinding& binding : material->textures)
                    {
                        const TextureRecord* texture = world.get(binding.texture);
                        const std::optional<SamplerRealizationKey> sampler = makeSamplerRealizationKey(binding.sampler);
                        if (!texture || !sampler)
                            return false;
                        const TextureRealizationKey textureKey = makeTextureRealizationKey(binding, *texture);
                        if (!textureKey.valid())
                            return false;
                        draw.textures.push_back(textureKey);
                        draw.samplers.push_back(*sampler);
                    }
                    plan.draws.push_back(std::move(draw));
                }
                return true;
            }

            if (node.kind == ModelNodeKind::Switch)
            {
                for (const ModelNodeIndex child : children[index.value()])
                {
                    if (node.activeSwitchChild && child == *node.activeSwitchChild)
                    {
                        if (!self(self, child, state))
                            return false;
                    }
                    else
                        static_plan_detail::countSubtree(payload, children, child, plan.switchSuppressedNodes);
                }
                return true;
            }

            if (node.kind == ModelNodeKind::Lod)
            {
                if (!node.lod)
                    return false;
                const std::optional<ModelNodeIndex> selected = selectModelLodChild(*node.lod, options.lodEyeDistance);
                for (const ModelNodeIndex child : children[index.value()])
                {
                    if (selected && child == *selected)
                    {
                        if (!self(self, child, state))
                            return false;
                    }
                    else
                        static_plan_detail::countSubtree(payload, children, child, plan.lodSuppressedNodes);
                }
                return true;
            }

            for (const ModelNodeIndex child : children[index.value()])
            {
                if (!self(self, child, state))
                    return false;
            }
            return true;
        };

        for (const ModelNodeIndex root : payload.roots)
        {
            if (!visit(visit, root, static_plan_detail::TraversalState{}))
                return std::nullopt;
        }
        return plan;
    }
}

#endif
