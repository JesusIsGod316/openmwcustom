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

    // Effective V3.25 drawable ordering after NiSortAdjustNode's loader-global
    // state machine has been applied. This is intentionally backend planning
    // state, not a source-format enum: VSG realization needs the final ordering
    // behavior, not a reconstructed NIF scene graph.
    enum class StaticDrawSortPolicy : std::uint8_t
    {
        Default,
        BackToFront,
        Traversal,
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
        StaticDrawSortPolicy sortPolicy = StaticDrawSortPolicy::Default;
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
        std::uint32_t backToFrontDraws = 0;
        std::uint32_t traversalOrderedDraws = 0;

        [[nodiscard]] bool empty() const noexcept { return draws.empty(); }
    };

    namespace static_plan_detail
    {
        struct TraversalState
        {
            glm::mat4 world{ 1.0f };
            std::optional<RenderCore::ModelBillboardMode> billboard;
        };

        // NifOsg::LoaderImpl intentionally keeps these as loader-global state;
        // NiSortAdjustNode is not pushed/popped with hierarchy traversal. Preserve
        // that realized V3.25 behavior exactly instead of treating sort nodes as
        // ordinary inherited parent state.
        struct LegacySortLoaderState
        {
            std::optional<RenderCore::ModelSortSemantic> pushed;
            std::optional<RenderCore::ModelSortSemantic> lastAppliedNoInherit;
            bool hasStencilProperty = false;
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

        [[nodiscard]] inline bool applySortNode(
            const RenderCore::ModelNodeRecord& node, LegacySortLoaderState& state) noexcept
        {
            if (!node.sort)
                return true;

            const RenderCore::ModelSortSemantic& sort = *node.sort;
            if (sort.accumulator == RenderCore::ModelSortAccumulator::Unsupported)
                return false;

            // V3.25 warns for a missing accumulator and leaves mPushedSorter
            // unchanged. In particular, an authored Off node without an
            // accumulator does NOT disable the previously selected sorter.
            if (sort.accumulator == RenderCore::ModelSortAccumulator::Missing)
                return true;

            if (state.pushed && state.pushed->accumulator != RenderCore::ModelSortAccumulator::Missing
                && state.pushed->mode != RenderCore::ModelSortMode::Inherit)
                state.lastAppliedNoInherit = state.pushed;

            state.pushed = sort;
            return true;
        }

        [[nodiscard]] inline std::optional<StaticDrawSortPolicy> accumulatorSortPolicy(
            RenderCore::ModelSortMode mode, RenderCore::ModelSortAccumulator accumulator, bool hasSortAlpha) noexcept
        {
            // V3.25's assignBin() handles Off before inspecting the accumulator.
            if (mode == RenderCore::ModelSortMode::Off)
                return StaticDrawSortPolicy::Traversal;

            switch (accumulator)
            {
                case RenderCore::ModelSortAccumulator::Alpha:
                    return hasSortAlpha ? StaticDrawSortPolicy::BackToFront : StaticDrawSortPolicy::Traversal;
                case RenderCore::ModelSortAccumulator::Cluster:
                    return StaticDrawSortPolicy::BackToFront;
                case RenderCore::ModelSortAccumulator::Missing:
                case RenderCore::ModelSortAccumulator::Unsupported:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::optional<StaticDrawSortPolicy> materialSortPolicy(
            LegacySortLoaderState& state, const RenderCore::MaterialRecord& material) noexcept
        {
            // LoaderImpl flips this global bit as soon as an enabled stencil
            // property is handled. Its final no-sorter rule then overrides the
            // earlier decal bin for non-alpha-sorted drawables.
            if (material.stencil.enabled)
                state.hasStencilProperty = true;

            const bool hasSortAlpha = material.alphaBlendEnabled
                && material.transparentSort == RenderCore::TransparentSortPolicy::Sorted;

            if (!state.pushed)
            {
                if (!hasSortAlpha && state.hasStencilProperty)
                    return StaticDrawSortPolicy::Traversal;
                if (hasSortAlpha || material.decal)
                    return StaticDrawSortPolicy::BackToFront;
                return StaticDrawSortPolicy::Default;
            }

            const RenderCore::ModelSortSemantic* effective = &*state.pushed;
            if (effective->mode == RenderCore::ModelSortMode::Inherit)
            {
                if (state.lastAppliedNoInherit)
                    effective = &*state.lastAppliedNoInherit;
                else
                {
                    return accumulatorSortPolicy(
                        RenderCore::ModelSortMode::Inherit, RenderCore::ModelSortAccumulator::Alpha, hasSortAlpha);
                }
            }

            return accumulatorSortPolicy(effective->mode, effective->accumulator, hasSortAlpha);
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

        // First replay the source loader's global sort/stencil state over the
        // COMPLETE authored depth-first traversal. Hidden, inactive switch, and
        // unselected LOD branches still participate here because V3.25 builds
        // them and can let their NiSortAdjustNode state affect later siblings.
        // Visibility selection is intentionally a separate second pass.
        std::vector<std::vector<StaticDrawSortPolicy>> nodeMaterialSortPolicies(payload.nodes.size());
        static_plan_detail::LegacySortLoaderState sortState;
        const auto collectSort = [&](const auto& self, ModelNodeIndex index) -> bool {
            if (!index.valid() || index.value() >= payload.nodes.size())
                return false;
            const ModelNodeRecord& node = payload.nodes[index.value()];
            if (!static_plan_detail::applySortNode(node, sortState))
                return false;

            if (node.kind == ModelNodeKind::Geometry)
            {
                auto& policies = nodeMaterialSortPolicies[index.value()];
                policies.reserve(node.materials.size());
                for (const MaterialHandle handle : node.materials)
                {
                    const MaterialRecord* material = world.get(handle);
                    if (!material)
                        return false;
                    const auto policy = static_plan_detail::materialSortPolicy(sortState, *material);
                    if (!policy)
                        return false;
                    policies.push_back(*policy);
                }
            }

            for (const ModelNodeIndex child : children[index.value()])
            {
                if (!self(self, child))
                    return false;
            }
            return true;
        };

        for (const ModelNodeIndex root : payload.roots)
        {
            if (!collectSort(collectSort, root))
                return std::nullopt;
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

                const auto& materialSortPolicies = nodeMaterialSortPolicies[index.value()];
                if (materialSortPolicies.size() != node.materials.size())
                    return false;

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
                    draw.sortPolicy = materialSortPolicies[surface.materialSlot];
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
                    if (draw.sortPolicy == StaticDrawSortPolicy::BackToFront)
                        ++plan.backToFrontDraws;
                    else if (draw.sortPolicy == StaticDrawSortPolicy::Traversal)
                        ++plan.traversalOrderedDraws;
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
