#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDPLAN_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDPLAN_H

#include "staticassetplan.hpp"

#include <components/rendercore/renderworld.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
        const RenderCore::RenderWorld* sourceWorld = nullptr; // compared, never dereferenced
        RenderCore::WorldEpoch sourceEpoch;
        RenderCore::RenderWorldRevision assetRevision;
        RenderCore::ChunkHandle chunk;
        RenderCore::ResourceRevision chunkRevision;
        // Hint only: reordered/removed model groups must still resolve exactly.
        std::size_t groupIndex = std::numeric_limits<std::size_t>::max();
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
        std::uint32_t reusedPopulationPlans = 0;

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

        // OpenMW's named-switch callbacks are enabled by exact root user
        // descriptions, not node names alone. The source adapter publishes those
        // model-global capabilities on every placement so planning can stay
        // backend-neutral. Graphic herbalism additionally carries per-reference
        // harvested state.
        const bool nightDayCapable
            = (instance->semanticFlags & RenderCore::NightDaySwitchCapabilitySemanticFlag) != 0;
        const bool herbalismCapable
            = (instance->semanticFlags & RenderCore::HerbalismSwitchCapabilitySemanticFlag) != 0;
        options.dayNightSwitchesEnabled = options.dayNightSwitchesEnabled && nightDayCapable;
        options.herbalismHarvested = herbalismCapable
            && (instance->semanticFlags & RenderCore::HerbalismHarvestedSemanticFlag) != 0;
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
        const RenderCore::ChunkRecord* chunk = world.get(chunkHandle);
        const RenderCore::ModelRecord* model = world.get(population.model);
        if (!chunk || !chunk->revision.valid() || !chunk->population || population.instances.empty() || !model
            || !model->revision.valid())
            return std::nullopt;

        // One population group has exactly one model realization. Capability
        // bits must therefore agree for every placement; disagreement is a
        // producer bug and fails closed. Per-reference harvested state cannot be
        // represented by this grouped path and is likewise rejected.
        constexpr std::uint64_t capabilityMask = RenderCore::NightDaySwitchCapabilitySemanticFlag
            | RenderCore::HerbalismSwitchCapabilitySemanticFlag;
        const std::uint64_t capabilities = population.instances.front().semanticFlags & capabilityMask;
        for (const RenderCore::PopulationInstanceRecord& placement : population.instances)
        {
            if ((placement.semanticFlags & capabilityMask) != capabilities
                || (placement.semanticFlags & RenderCore::HerbalismHarvestedSemanticFlag) != 0)
                return std::nullopt;
        }
        options.dayNightSwitchesEnabled = options.dayNightSwitchesEnabled
            && (capabilities & RenderCore::NightDaySwitchCapabilitySemanticFlag) != 0;
        options.herbalismHarvested = false;

        std::optional<StaticAssetPlan> asset = buildStaticAssetPlan(world, population.model, options);
        if (!asset)
            return std::nullopt;
        StaticPopulationPlan result{
            .sourceWorld = &world,
            .sourceEpoch = world.epoch(),
            .assetRevision = world.assetRevision(),
            .chunk = chunkHandle,
            .chunkRevision = chunk->revision,
            .groupIndex = static_cast<std::size_t>(std::find_if(chunk->population->groups.begin(),
                chunk->population->groups.end(), [&](const auto& group) { return group.model == population.model; })
                - chunk->population->groups.begin()),
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

    struct PopulationStaleCause;
    [[nodiscard]] inline bool staticPopulationPlanCurrent(const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan, bool allowUnchangedGroup, PopulationStaleCause* cause);

    using PopulationPlanLookup = std::function<const StaticPopulationPlan*(RenderCore::ChunkHandle, RenderCore::ModelHandle)>;

    [[nodiscard]] inline StaticPlanOptions populationPlanOptions(
        StaticPlanOptions options, const RenderCore::ModelPopulationRecord& population)
    {
        options.includeDeformableMeshes = false;
        options.herbalismHarvested = false;
        options.dayNightSwitchesEnabled = options.dayNightSwitchesEnabled && !population.instances.empty()
            && (population.instances.front().semanticFlags & RenderCore::NightDaySwitchCapabilitySemanticFlag) != 0;
        return options;
    }

    // Deterministic production discovery pass. Unsupported CP3D/CP4 populations
    // are counted explicitly, while malformed static-model instances make the
    // plan invalid instead of disappearing from the Vulkan scene silently.
    [[nodiscard]] inline StaticWorldPlan buildStaticWorldPlan(
        const RenderCore::RenderWorld& world, StaticPlanOptions options = {}, PopulationPlanLookup previous = {},
        bool includePopulations = true)
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
        if (!includePopulations) return result;
        world.forEachChunk([&](RenderCore::ChunkHandle handle, const RenderCore::ChunkRecord& chunk) {
            if (!chunk.population)
                return;
            for (const RenderCore::ModelPopulationRecord& population : chunk.population->groups)
            {
                // Reuse only exact, still-current authored dependencies. A dirty
                // group must not force the unrelated groups through asset-plan
                // reconstruction. Discovery/order and malformed admission stay live.
                auto effectiveOptions = options;
                effectiveOptions.includeDeformableMeshes = false;
                effectiveOptions.herbalismHarvested = false;
                effectiveOptions.dayNightSwitchesEnabled = effectiveOptions.dayNightSwitchesEnabled
                    && !population.instances.empty() && (population.instances.front().semanticFlags
                        & RenderCore::NightDaySwitchCapabilitySemanticFlag) != 0;
                if (const auto* prior = previous ? previous(handle, population.model) : nullptr;
                    prior && prior->options == effectiveOptions && staticPopulationPlanCurrent(world, *prior, true, nullptr))
                {
                    result.populations.push_back(*prior);
                    result.populations.back().chunkRevision = chunk.revision;
                    result.populations.back().sourceWorld = &world;
                    result.populations.back().sourceEpoch = world.epoch();
                    result.populations.back().assetRevision = world.assetRevision();
                    ++result.reusedPopulationPlans;
                    continue;
                }
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

    [[nodiscard]] inline bool equivalentPopulationPlacement(const RenderCore::PopulationInstanceRecord& left,
        const RenderCore::PopulationInstanceRecord& right) noexcept
    {
        return left.sourceIdentity == right.sourceIdentity && left.transform.translation == right.transform.translation
            && left.transform.rotation == right.transform.rotation && left.transform.scale == right.transform.scale
            && left.localBounds.minimum == right.localBounds.minimum && left.localBounds.maximum == right.localBounds.maximum
            && left.lod.center == right.lod.center && left.lod.minimumDistance == right.lod.minimumDistance
            && left.lod.maximumDistance == right.lod.maximumDistance && left.lod.scale == right.lod.scale
            && left.lod.smallFeatureEligible == right.lod.smallFeatureEligible
            && left.semanticFlags == right.semanticFlags && left.lightingEnabled == right.lightingEnabled;
    }

    struct PopulationStaleCause
    {
        const char* field = "none";
        std::string source;
        std::uint64_t oldRevision = 0, newRevision = 0;
        std::string chunkIdentity;
        std::string modelIdentity;
    };

    // Geometry/material ownership excludes placement. Compare the complete
    // resource dependency contract, not the chunk revision (which movement
    // advances). This is only usable for non-instanced, placement-free assets.
    [[nodiscard]] inline bool reusablePopulationAsset(
        const StaticPopulationPlan& previous, const StaticPopulationPlan& next)
    {
        return previous.sourceWorld == next.sourceWorld && previous.sourceEpoch == next.sourceEpoch
            && previous.model == next.model && previous.modelRevision == next.modelRevision
            && previous.options == next.options && previous.meshes == next.meshes
            && previous.materials == next.materials && previous.textures == next.textures;
    }

    [[nodiscard]] inline bool staticPopulationPlanCurrent(const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan, bool allowUnchangedGroup = false,
        PopulationStaleCause* cause = nullptr)
    {
        const auto stale = [&](const char* field, std::string_view source = {},
                               std::uint64_t oldRevision = 0, std::uint64_t newRevision = 0) {
            if (cause)
            {
                *cause = {field, std::string(source), oldRevision, newRevision, {}, {}};
                if (const auto* owner = world.get(plan.chunk)) cause->chunkIdentity = owner->producerIdentity;
                if (const auto* owner = world.get(plan.model)) cause->modelIdentity = owner->sourceIdentity;
            }
            return false;
        };
        const RenderCore::ChunkRecord* chunk = world.get(plan.chunk);
        const RenderCore::ModelRecord* model = world.get(plan.model);
        if (!chunk || !chunk->population) return stale("chunk_missing");
        if (!model) return stale("model_missing");
        if (!allowUnchangedGroup && chunk->revision != plan.chunkRevision)
            return stale("chunk_revision", {}, plan.chunkRevision.value(), chunk->revision.value());
        if (model->revision != plan.modelRevision)
            return stale("model_revision", model->sourceIdentity, plan.modelRevision.value(), model->revision.value());
        if (chunk->kind != plan.kind) return stale("chunk_kind");
        // A successful build/check established the group and dependencies.
        // Actor/light/placement changes cannot mutate model assets. Keep the
        // exact full dependency walk as the independently selectable control.
        const bool revisionGate = std::getenv("OPENMW_V4_STATIC_REVISION_GATE")
            && plan.sourceWorld == &world && plan.sourceEpoch == world.epoch();
        const bool assetsCurrent = revisionGate && plan.assetRevision == world.assetRevision();
        if (assetsCurrent && chunk->revision == plan.chunkRevision) return true;
        const auto& groups = chunk->population->groups;
        const auto group = plan.groupIndex < groups.size() && groups[plan.groupIndex].model == plan.model
            ? groups.begin() + plan.groupIndex
            : std::find_if(groups.begin(), groups.end(),
                [&](const RenderCore::ModelPopulationRecord& value) { return value.model == plan.model; });
        if (group == chunk->population->groups.end()) return stale("group_missing", model->sourceIdentity);
        if (group->instances.size() != plan.placements.size())
            return stale("placement_count", model->sourceIdentity);
        // A cell revision covers ALL model groups. Live placement insertion or
        // removal (including script-driven vegetation) must not rebuild every
        // unrelated model in that cell. Compare exact authored inputs, not a
        // hash or a tolerance. An unchanged group can retain its old packing
        // origin: the root translation and packed transforms still describe
        // the same absolute placements, even if the cell bounds have grown.
        // Strict revision validation remains the default for new commits.
        if (chunk->revision != plan.chunkRevision
            && !std::equal(group->instances.begin(), group->instances.end(), plan.placements.begin(),
                equivalentPopulationPlacement))
            return stale("placement_fields", model->sourceIdentity, plan.chunkRevision.value(), chunk->revision.value());
        if (assetsCurrent) return true;
        for (const auto& dependency : plan.meshes)
        {
            const RenderCore::MeshRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return stale("mesh_revision", record ? record->sourceIdentity : "", dependency.revision.value(),
                    record ? record->revision.value() : 0);
        }
        for (const auto& dependency : plan.materials)
        {
            const RenderCore::MaterialRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return stale("material_revision", record ? record->sourceIdentity : "", dependency.revision.value(),
                    record ? record->revision.value() : 0);
        }
        for (const auto& dependency : plan.textures)
        {
            const RenderCore::TextureRecord* record = world.get(dependency.handle);
            if (!record || record->revision != dependency.revision)
                return stale("texture_revision", record ? record->sourceIdentity : "", dependency.revision.value(),
                    record ? record->revision.value() : 0);
        }
        return true;
    }
}

#endif
