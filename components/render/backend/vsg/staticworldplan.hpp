#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDPLAN_H

#include "staticassetplan.hpp"

#include <components/rendercore/renderworld.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RenderVsg
{
    template <class Handle>
    struct StaticResourceDependency
    {
        Handle handle;
        RenderCore::ResourceRevision revision;

        friend bool operator==(const StaticResourceDependency&, const StaticResourceDependency&) = default;
    };

    struct StaticInstancePlan
    {
        RenderCore::InstanceHandle instance;
        RenderCore::ResourceRevision instanceRevision;
        RenderCore::ModelHandle model;
        RenderCore::ResourceRevision modelRevision;
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

    struct StaticPopulationPlan
    {
        RenderCore::ChunkHandle chunk;
        RenderCore::ResourceRevision chunkRevision;
        RenderCore::ChunkRecord::Kind kind = RenderCore::ChunkRecord::Kind::StaticPopulation;
        RenderCore::ModelHandle model;
        RenderCore::ResourceRevision modelRevision;
        RenderCore::WorldPosition coordinateOrigin;
        std::vector<RenderCore::PopulationInstanceRecord> placements;
        StaticPlanOptions options;
        StaticAssetPlan asset;
        std::vector<StaticResourceDependency<RenderCore::MeshHandle>> meshes;
        std::vector<StaticResourceDependency<RenderCore::MaterialHandle>> materials;
        std::vector<StaticResourceDependency<RenderCore::TextureHandle>> textures;
    };

    struct StaticWorldPlan
    {
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        std::vector<StaticInstancePlan> instances;
        std::vector<StaticPopulationPlan> populations;
        std::uint32_t simpleMeshInstancesDeferred = 0;
        std::uint32_t dynamicInstancesDeferred = 0;
        std::uint32_t invalidModelInstances = 0;
        std::uint32_t invalidPopulationGroups = 0;

        [[nodiscard]] bool valid() const noexcept
        {
            return worldEpoch.valid() && worldRevision.valid() && invalidModelInstances == 0
                && invalidPopulationGroups == 0;
        }
    };

    // Preserve double-precision world translation until the backend placement
    // boundary. Model-local geometry remains float, while camera-relative GPU
    // packing can later replace this matrix conversion without changing the
    // semantic producer or persistent instance identity.
    [[nodiscard]] inline glm::dmat4 staticInstancePlacementMatrix(
        const RenderCore::WorldTransform& transform) noexcept
    {
        glm::dmat4 result = glm::translate(glm::dmat4(1.0), transform.translation);
        result *= glm::mat4_cast(glm::dquat(transform.rotation));
        return glm::scale(result, glm::dvec3(transform.scale));
    }

    [[nodiscard]] inline std::optional<StaticInstancePlan> buildStaticInstancePlan(
        const RenderCore::RenderWorld& world, RenderCore::InstanceHandle handle, StaticPlanOptions options = {})
    {
        options.includeDeformableMeshes = false;
        const RenderCore::InstanceRecord* instance = world.get(handle);
        if (!instance || !instance->revision.valid() || !instance->model || instance->mesh.valid()
            || instance->skeleton || instance->attachment)
            return std::nullopt;

        const RenderCore::ModelRecord* model = world.get(*instance->model);
        if (!model || !model->revision.valid())
            return std::nullopt;

        // Graphic herbalism is reference state, not model state. Derive the
        // realization option from this exact instance so two organic containers
        // sharing one model may legitimately select different switch children.
        options.herbalismHarvested
            = (instance->semanticFlags & RenderCore::HerbalismHarvestedSemanticFlag) != 0;
        std::optional<StaticAssetPlan> asset = buildStaticAssetPlan(world, *instance->model, options);
        if (!asset)
            return std::nullopt;

        StaticInstancePlan result{
            .instance = handle,
            .instanceRevision = instance->revision,
            .model = *instance->model,
            .modelRevision = model->revision,
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
                                   Handle dependency, RenderCore::ResourceRevision revision) {
            const StaticResourceDependency<Handle> candidate{ dependency, revision };
            if (std::find(dependencies.begin(), dependencies.end(), candidate) == dependencies.end())
                dependencies.push_back(candidate);
        };
        for (const StaticDrawPlan& draw : result.asset.draws)
        {
            const RenderCore::MeshRecord* mesh = world.get(draw.mesh);
            const RenderCore::MaterialRecord* material = world.get(draw.material);
            if (!mesh || !material)
                return std::nullopt;
            addUnique(result.meshes, draw.mesh, mesh->revision);
            addUnique(result.materials, draw.material, material->revision);
            for (const RenderCore::TextureRealizationKey& texture : draw.textures)
                addUnique(result.textures, texture.view.texture, texture.revision);
        }
        return result;
    }

    [[nodiscard]] inline std::optional<StaticPopulationPlan> buildStaticPopulationPlan(
        const RenderCore::RenderWorld& world, RenderCore::ChunkHandle chunkHandle,
        const RenderCore::ModelPopulationRecord& population, StaticPlanOptions options = {})
    {
        options.includeDeformableMeshes = false;
        // Population groups share one immutable model draw plan. Per-reference
        // herbalism state is intentionally unavailable here; interactive
        // useAnim references stay individually addressable instead.
        options.herbalismHarvested = false;
        const RenderCore::ChunkRecord* chunk = world.get(chunkHandle);
        const RenderCore::ModelRecord* model = world.get(population.model);
        if (!chunk || !chunk->revision.valid() || !chunk->population || population.instances.empty() || !model
            || !model->revision.valid())
            return std::nullopt;

        std::optional<StaticAssetPlan> asset = buildStaticAssetPlan(world, population.model, options);
        if (!asset)
            return std::nullopt;
        StaticPopulationPlan result{
            .chunk = chunkHandle,
            .chunkRevision = chunk->revision,
            .kind = chunk->kind,
            .model = population.model,
            .modelRevision = model->revision,
            .coordinateOrigin = glm::dvec3(chunk->bounds.minimum)
                + (glm::dvec3(chunk->bounds.maximum) - glm::dvec3(chunk->bounds.minimum)) * 0.5,
            .placements = population.instances,
            .options = options,
            .asset = std::move(*asset),
            .meshes = {},
            .materials = {},
            .textures = {},
        };
        const auto addUnique = []<class Handle>(std::vector<StaticResourceDependency<Handle>>& dependencies,
                                   Handle dependency, RenderCore::ResourceRevision revision) {
            const StaticResourceDependency<Handle> candidate{ dependency, revision };
            if (std::find(dependencies.begin(), dependencies.end(), candidate) == dependencies.end())
                dependencies.push_back(candidate);
        };
        for (const StaticDrawPlan& draw : result.asset.draws)
        {
            const RenderCore::MeshRecord* mesh = world.get(draw.mesh);
            const RenderCore::MaterialRecord* material = world.get(draw.material);
            if (!mesh || !material)
                return std::nullopt;
            addUnique(result.meshes, draw.mesh, mesh->revision);
            addUnique(result.materials, draw.material, material->revision);
            for (const RenderCore::TextureRealizationKey& texture : draw.textures)
                addUnique(result.textures, texture.view.texture, texture.revision);
        }
        return result;
    }

    // Deterministic production discovery pass. Unsupported CP3D/CP4 populations
    // are counted explicitly, while malformed static-model instances make the
    // plan invalid instead of disappearing from the Vulkan scene silently.
    [[nodiscard]] inline StaticWorldPlan buildStaticWorldPlan(
        const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
    {
        StaticWorldPlan result;
        result.worldEpoch = world.epoch();
        result.worldRevision = world.revision();
        result.instances.reserve(world.instanceCount());

        world.forEachInstance([&](RenderCore::InstanceHandle handle, const RenderCore::InstanceRecord& instance) {
            if (instance.mesh.valid() && !instance.model)
            {
                ++result.simpleMeshInstancesDeferred;
                return;
            }
            if (instance.skeleton || instance.attachment)
            {
                ++result.dynamicInstancesDeferred;
                return;
            }

            std::optional<StaticInstancePlan> plan = buildStaticInstancePlan(world, handle, options);
            if (!plan)
            {
                ++result.invalidModelInstances;
                return;
            }
            result.instances.push_back(std::move(*plan));
        });
        world.forEachChunk([&](RenderCore::ChunkHandle handle, const RenderCore::ChunkRecord& chunk) {
            if (!chunk.population)
                return;
            for (const RenderCore::ModelPopulationRecord& population : chunk.population->groups)
            {
                std::optional<StaticPopulationPlan> plan
                    = buildStaticPopulationPlan(world, handle, population, options);
                if (!plan)
                {
                    ++result.invalidPopulationGroups;
                    continue;
                }
                result.populations.push_back(std::move(*plan));
            }
        });
        return result;
    }

    [[nodiscard]] inline bool staticInstancePlanCurrent(
        const RenderCore::RenderWorld& world, const StaticInstancePlan& plan) noexcept
    {
        const RenderCore::InstanceRecord* instance = world.get(plan.instance);
        const RenderCore::ModelRecord* model = world.get(plan.model);
        if (!instance || !model || instance->revision != plan.instanceRevision || instance->model != plan.model
            || model->revision != plan.modelRevision)
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

    [[nodiscard]] inline bool staticPopulationPlanCurrent(
        const RenderCore::RenderWorld& world, const StaticPopulationPlan& plan) noexcept
    {
        const RenderCore::ChunkRecord* chunk = world.get(plan.chunk);
        const RenderCore::ModelRecord* model = world.get(plan.model);
        if (!chunk || !model || chunk->revision != plan.chunkRevision || !chunk->population
            || model->revision != plan.modelRevision || chunk->kind != plan.kind)
            return false;
        const auto group = std::find_if(chunk->population->groups.begin(), chunk->population->groups.end(),
            [&](const RenderCore::ModelPopulationRecord& value) { return value.model == plan.model; });
        if (group == chunk->population->groups.end() || group->instances.size() != plan.placements.size())
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
}

#endif
