#ifndef OPENMW_COMPONENTS_RENDERCORE_STATICPOPULATIONPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_STATICPOPULATIONPRODUCER_H

#include "updatebatch.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RenderCore
{
    struct StaticPopulationCellSource
    {
        std::string identity;
        std::string worldspaceIdentity;
        AxisAlignedBounds bounds;
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
        bool groundcover = false;
    };

    struct StaticPopulationInstanceSource
    {
        std::string identity;
        std::string cellIdentity;
        ModelHandle model;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        LodSemantic lod;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster) | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    enum class StaticPopulationPublishStatus : std::uint8_t
    {
        Applied,
        AlreadyPresent,
        InvalidSource,
        ReservationFailed,
        SequenceExhausted,
        BatchBuildFailed,
        PublishRejected,
    };

    // Stages independently addressable gameplay objects, then publishes one
    // immutable, model-grouped transform payload per exterior cell (control) or
    // per cell/model group. A moved object then rebuilds only its own model's
    // placements, not every building/tree in the cell. The staging
    // map is source-side bookkeeping; RenderWorld and the backend never allocate
    // one authoritative InstanceRecord/VSG node per placement.
    class StaticPopulationProducer final
    {
    public:
        StaticPopulationProducer(RenderWorld& world, RenderWorldPublisher& publisher, bool groupPublication = false)
            : mWorld(world)
            , mPublisher(publisher)
            , mEpoch(world.epoch())
            , mGroupPublication(groupPublication)
        {
        }

        [[nodiscard]] StaticPopulationPublishStatus addCell(StaticPopulationCellSource source)
        {
            synchronizeEpoch();
            if (!valid(source))
                return StaticPopulationPublishStatus::InvalidSource;
            if (const auto existing = mCells.find(source.identity); existing != mCells.end())
            {
                const StaticPopulationCellSource& current = existing->second.source;
                return current.worldspaceIdentity == source.worldspaceIdentity
                        && current.bounds.minimum == source.bounds.minimum
                        && current.bounds.maximum == source.bounds.maximum && current.gridX == source.gridX
                        && current.gridY == source.gridY && current.groundcover == source.groundcover
                    ? StaticPopulationPublishStatus::AlreadyPresent
                    : StaticPopulationPublishStatus::InvalidSource;
            }
            std::string identity = source.identity;
            mCells.emplace(std::move(identity), Cell{
                                                    .source = std::move(source),
                                                    .groups = {},
                                                });
            return StaticPopulationPublishStatus::Applied;
        }

        [[nodiscard]] StaticPopulationPublishStatus upsert(StaticPopulationInstanceSource source)
        {
            synchronizeEpoch();
            if (!valid(source) || !mWorld.get(source.model))
                return StaticPopulationPublishStatus::InvalidSource;
            const auto cell = mCells.find(source.cellIdentity);
            if (cell == mCells.end())
                return StaticPopulationPublishStatus::InvalidSource;

            const std::uint64_t groupKey = mGroupPublication ? modelKey(source.model) : 0;
            const auto owner = mOwners.find(source.identity);
            if (owner != mOwners.end())
            {
                const auto previous = mCells.find(owner->second.cell);
                if (previous == mCells.end())
                    return StaticPopulationPublishStatus::PublishRejected;
                const auto group = previous->second.groups.find(owner->second.group);
                if (group == previous->second.groups.end())
                    return StaticPopulationPublishStatus::PublishRejected;
                const auto existing = group->second.instances.find(source.identity);
                if (existing == group->second.instances.end())
                    return StaticPopulationPublishStatus::PublishRejected;
                if (equivalent(existing->second, source))
                    return StaticPopulationPublishStatus::AlreadyPresent;
                if (owner->second.cell != source.cellIdentity || owner->second.group != groupKey)
                {
                    group->second.instances.erase(existing);
                    group->second.dirty = true;
                }
            }
            std::string sourceIdentity = source.identity;
            Group& group = cell->second.groups[groupKey];
            group.instances.insert_or_assign(sourceIdentity, std::move(source));
            mOwners.insert_or_assign(std::move(sourceIdentity), Owner{cell->first, groupKey});
            group.dirty = true;
            return StaticPopulationPublishStatus::Applied;
        }

        [[nodiscard]] StaticPopulationPublishStatus remove(std::string_view identity)
        {
            synchronizeEpoch();
            const auto owner = mOwners.find(identity);
            if (owner == mOwners.end())
                return StaticPopulationPublishStatus::AlreadyPresent;
            const auto cell = mCells.find(owner->second.cell);
            if (cell == mCells.end())
                return StaticPopulationPublishStatus::PublishRejected;
            const auto group = cell->second.groups.find(owner->second.group);
            if (group == cell->second.groups.end())
                return StaticPopulationPublishStatus::PublishRejected;
            const auto instance = group->second.instances.find(identity);
            if (instance == group->second.instances.end())
                return StaticPopulationPublishStatus::PublishRejected;
            group->second.instances.erase(instance);
            group->second.dirty = true;
            mOwners.erase(owner);
            return StaticPopulationPublishStatus::Applied;
        }

        [[nodiscard]] StaticPopulationPublishStatus removeCell(std::string_view identity)
        {
            synchronizeEpoch();
            const auto cell = mCells.find(identity);
            if (cell == mCells.end())
                return StaticPopulationPublishStatus::AlreadyPresent;
            const bool published = std::ranges::any_of(cell->second.groups,
                [](const auto& entry) { return entry.second.handle.has_value(); });
            if (published)
            {
                RenderWorldUpdateBatch batch(mWorld.epoch(), mPublisher.nextSequence(), cell->first);
                if (!batch.sequence().valid())
                    return StaticPopulationPublishStatus::SequenceExhausted;
                for (const auto& [key, group] : cell->second.groups)
                    if (group.handle && !batch.add(RetireChunk{*group.handle}))
                        return StaticPopulationPublishStatus::BatchBuildFailed;
                if (!batch.seal())
                    return StaticPopulationPublishStatus::BatchBuildFailed;
                if (mPublisher.apply(batch) != PublishStatus::Applied)
                    return StaticPopulationPublishStatus::PublishRejected;
            }
            for (const auto& [key, group] : cell->second.groups)
                for (const auto& [sourceIdentity, source] : group.instances)
                    mOwners.erase(sourceIdentity);
            mCells.erase(cell);
            return StaticPopulationPublishStatus::Applied;
        }

        [[nodiscard]] StaticPopulationPublishStatus flush()
        {
            synchronizeEpoch();
            std::vector<std::pair<Group*, ChunkHandle>> creations;
            for (auto& [identity, sourceCell] : mCells)
            for (auto& [key, cell] : sourceCell.groups)
            {
                if (cell.dirty && cell.instances.empty() && !cell.handle)
                {
                    cell.dirty = false;
                    continue;
                }
                if (!cell.dirty || cell.instances.empty() || cell.handle)
                    continue;
                const std::optional<ChunkHandle> handle = mWorld.reserveChunk();
                if (!handle)
                {
                    cancel(creations);
                    return StaticPopulationPublishStatus::ReservationFailed;
                }
                creations.emplace_back(&cell, *handle);
            }

            const UpdateSequence sequence = mPublisher.nextSequence();
            if (!sequence.valid())
            {
                cancel(creations);
                return StaticPopulationPublishStatus::SequenceExhausted;
            }
            RenderWorldUpdateBatch batch(mWorld.epoch(), sequence, "static-populations");
            bool changed = false;
            for (auto& [identity, sourceCell] : mCells)
            for (auto& [key, cell] : sourceCell.groups)
            {
                if (!cell.dirty)
                    continue;
                if (cell.instances.empty())
                {
                    if (cell.handle)
                    {
                        changed = true;
                        if (!batch.add(RetireChunk{ *cell.handle }))
                        {
                            cancel(creations);
                            return StaticPopulationPublishStatus::BatchBuildFailed;
                        }
                    }
                    continue;
                }

                ChunkHandle handle = cell.handle.value_or(findCreation(creations, cell));
                ChunkRecord record = makeRecord(sourceCell.source, cell);
                if (mGroupPublication)
                    record.producerIdentity += ":model:" + std::to_string(key);
                changed = true;
                if (cell.handle)
                {
                    const ChunkRecord* current = mWorld.get(*cell.handle);
                    const std::optional<ResourceRevision> revision
                        = current ? advanceMonotonic(current->revision) : std::nullopt;
                    if (!revision)
                    {
                        cancel(creations);
                        return StaticPopulationPublishStatus::BatchBuildFailed;
                    }
                    record.revision = *revision;
                    if (!batch.add(UpdateChunk{ handle, std::move(record) }))
                    {
                        cancel(creations);
                        return StaticPopulationPublishStatus::BatchBuildFailed;
                    }
                }
                else if (!batch.add(CreateChunk{ handle, std::move(record) }))
                {
                    cancel(creations);
                    return StaticPopulationPublishStatus::BatchBuildFailed;
                }
            }
            if (!changed)
            {
                cancel(creations);
                for (auto& [identity, cell] : mCells)
                    std::erase_if(cell.groups, [](const auto& entry) {
                        return entry.second.instances.empty() && !entry.second.handle;
                    });
                return StaticPopulationPublishStatus::AlreadyPresent;
            }
            if (!batch.seal())
            {
                cancel(creations);
                return StaticPopulationPublishStatus::BatchBuildFailed;
            }
            if (mPublisher.apply(batch) != PublishStatus::Applied)
            {
                cancel(creations);
                return StaticPopulationPublishStatus::PublishRejected;
            }
            for (auto& [cell, handle] : creations)
                cell->handle = handle;
            for (auto& [identity, sourceCell] : mCells)
            {
                for (auto& [key, cell] : sourceCell.groups)
                {
                    if (cell.dirty && cell.instances.empty())
                        cell.handle.reset();
                    cell.dirty = false;
                }
                std::erase_if(sourceCell.groups, [](const auto& entry) {
                    return entry.second.instances.empty() && !entry.second.handle;
                });
            }
            return StaticPopulationPublishStatus::Applied;
        }

        [[nodiscard]] std::size_t stagedInstanceCount()
        {
            synchronizeEpoch();
            return mOwners.size();
        }

    private:
        struct Group
        {
            std::map<std::string, StaticPopulationInstanceSource, std::less<>> instances;
            std::optional<ChunkHandle> handle;
            bool dirty = false;
        };
        struct Cell
        {
            StaticPopulationCellSource source;
            std::map<std::uint64_t, Group> groups;
        };
        struct Owner
        {
            std::string cell;
            std::uint64_t group;
        };

        [[nodiscard]] static bool valid(const StaticPopulationCellSource& source) noexcept
        {
            return !source.identity.empty() && !source.worldspaceIdentity.empty()
                && semantic_detail::finite(source.bounds.minimum) && semantic_detail::finite(source.bounds.maximum)
                && source.bounds.minimum.x <= source.bounds.maximum.x
                && source.bounds.minimum.y <= source.bounds.maximum.y
                && source.bounds.minimum.z <= source.bounds.maximum.z;
        }

        [[nodiscard]] static bool valid(const StaticPopulationInstanceSource& source) noexcept
        {
            return !source.identity.empty() && !source.cellIdentity.empty() && source.model.valid()
                && semantic_detail::finite(source.transform.translation) && semantic_detail::finite(source.transform.rotation)
                && semantic_detail::finite(source.transform.scale) && semantic_detail::finite(source.localBounds.minimum)
                && semantic_detail::finite(source.localBounds.maximum)
                && source.localBounds.minimum.x <= source.localBounds.maximum.x
                && source.localBounds.minimum.y <= source.localBounds.maximum.y
                && source.localBounds.minimum.z <= source.localBounds.maximum.z
                && semantic_detail::finite(source.lod.center) && std::isfinite(source.lod.minimumDistance)
                && std::isfinite(source.lod.maximumDistance) && std::isfinite(source.lod.scale)
                && source.lod.minimumDistance >= 0.0f && source.lod.maximumDistance >= source.lod.minimumDistance
                && source.lod.scale > 0.0f;
        }

        [[nodiscard]] static bool equivalent(
            const StaticPopulationInstanceSource& left, const StaticPopulationInstanceSource& right) noexcept
        {
            return left.identity == right.identity && left.cellIdentity == right.cellIdentity && left.model == right.model
                && left.transform.translation == right.transform.translation
                && left.transform.rotation == right.transform.rotation && left.transform.scale == right.transform.scale
                && left.localBounds.minimum == right.localBounds.minimum
                && left.localBounds.maximum == right.localBounds.maximum && left.lod.center == right.lod.center
                && left.lod.minimumDistance == right.lod.minimumDistance
                && left.lod.maximumDistance == right.lod.maximumDistance && left.lod.scale == right.lod.scale
                && left.lod.smallFeatureEligible == right.lod.smallFeatureEligible
                && left.semanticFlags == right.semanticFlags && left.lightingEnabled == right.lightingEnabled;
        }

        [[nodiscard]] static std::uint64_t modelKey(ModelHandle handle) noexcept
        {
            return (static_cast<std::uint64_t>(handle.generation()) << 32u) | handle.slot();
        }

        [[nodiscard]] static ChunkRecord makeRecord(const StaticPopulationCellSource& sourceCell, const Group& cell)
        {
            std::map<std::uint64_t, ModelPopulationRecord> groups;
            AxisAlignedBounds bounds;
            bounds.minimum = glm::vec3(std::numeric_limits<float>::max());
            bounds.maximum = glm::vec3(std::numeric_limits<float>::lowest());
            for (const auto& [identity, source] : cell.instances)
            {
                ModelPopulationRecord& group = groups[modelKey(source.model)];
                group.model = source.model;
                std::uint64_t semanticFlags = source.semanticFlags;
                if (sourceCell.groundcover)
                    semanticFlags |= semanticFlag(InstanceSemanticFlag::Groundcover);
                group.instances.push_back({ .sourceIdentity = identity,
                    .transform = source.transform,
                    .localBounds = source.localBounds,
                    .lod = source.lod,
                    .semanticFlags = semanticFlags,
                    .lightingEnabled = source.lightingEnabled });
                for (unsigned int corner = 0; corner < 8; ++corner)
                {
                    const glm::vec3 local{
                        (corner & 1u) != 0 ? source.localBounds.maximum.x : source.localBounds.minimum.x,
                        (corner & 2u) != 0 ? source.localBounds.maximum.y : source.localBounds.minimum.y,
                        (corner & 4u) != 0 ? source.localBounds.maximum.z : source.localBounds.minimum.z,
                    };
                    const glm::dvec3 scaled = glm::dvec3(local * source.transform.scale);
                    const glm::dvec3 world
                        = source.transform.translation + glm::dquat(source.transform.rotation) * scaled;
                    const glm::vec3 packed(world);
                    bounds.minimum = glm::min(bounds.minimum, packed);
                    bounds.maximum = glm::max(bounds.maximum, packed);
                }
            }
            auto payload = std::make_shared<StaticPopulationPayload>();
            payload->groups.reserve(groups.size());
            for (auto& [key, group] : groups)
                payload->groups.push_back(std::move(group));

            ChunkRecord record;
            record.producerIdentity = "population:" + sourceCell.identity;
            record.worldspaceIdentity = sourceCell.worldspaceIdentity;
            record.bounds = bounds;
            record.kind = sourceCell.groundcover ? ChunkRecord::Kind::Groundcover : ChunkRecord::Kind::StaticPopulation;
            record.gridX = sourceCell.gridX;
            record.gridY = sourceCell.gridY;
            record.population = std::move(payload);
            if (sourceCell.groundcover)
                record.semanticFlags |= semanticFlag(InstanceSemanticFlag::Groundcover);
            return record;
        }

        [[nodiscard]] static ChunkHandle findCreation(
            const std::vector<std::pair<Group*, ChunkHandle>>& creations, const Group& cell) noexcept
        {
            const auto found = std::ranges::find_if(creations, [&](const auto& entry) { return entry.first == &cell; });
            return found == creations.end() ? ChunkHandle{} : found->second;
        }

        void cancel(const std::vector<std::pair<Group*, ChunkHandle>>& creations) noexcept
        {
            for (const auto& [cell, handle] : creations)
                static_cast<void>(mWorld.cancel(handle));
        }

        void synchronizeEpoch()
        {
            if (mEpoch == mWorld.epoch())
                return;
            mCells.clear();
            mOwners.clear();
            mEpoch = mWorld.epoch();
        }

        RenderWorld& mWorld;
        RenderWorldPublisher& mPublisher;
        WorldEpoch mEpoch;
        bool mGroupPublication = false;
        std::map<std::string, Cell, std::less<>> mCells;
        std::map<std::string, Owner, std::less<>> mOwners;
    };
}

#endif
