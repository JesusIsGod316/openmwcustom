#ifndef OPENMW_COMPONENTS_RENDERCORE_ACTIVECELLPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_ACTIVECELLPRODUCER_H

#include "updatebatch.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RenderCore
{
    struct ActiveCellSource
    {
        std::string identity;
        std::string worldspaceIdentity;
        AxisAlignedBounds bounds;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld);
    };

    struct StaticInstanceSource
    {
        // Stable engine identity, normally derived from the authoritative
        // content-file/refnum pair rather than an address or an OSG node.
        std::string identity;
        std::string cellIdentity;
        ModelHandle model;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        LodSemantic lod;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster)
            | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    struct DynamicInstanceSource
    {
        std::string identity;
        std::string cellIdentity;
        ModelHandle model;
        SkeletonHandle skeleton;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        LodSemantic lod;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster)
            | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    struct CellLightSource
    {
        std::string identity;
        std::string cellIdentity;
        LightRecord light;
    };

    enum class ActiveCellPublishStatus : std::uint8_t
    {
        Applied,
        AlreadyPresent,
        NotFound,
        InvalidSource,
        MissingCell,
        MissingModel,
        MissingSkeleton,
        ReservationFailed,
        SequenceExhausted,
        ResourceRevisionExhausted,
        BatchBuildFailed,
        PublishRejected,
    };

    struct ActiveCellPublishResult
    {
        ActiveCellPublishStatus status = ActiveCellPublishStatus::InvalidSource;
        PublishStatus worldStatus = PublishStatus::OperationRejected;
        ChunkHandle chunk;
        InstanceHandle instance;
        LightHandle light;

        [[nodiscard]] bool applied() const noexcept { return status == ActiveCellPublishStatus::Applied; }
    };

    // Main-thread semantic producer for the authoritative active-cell lifecycle.
    // The game/source adapter resolves winning VFS models before this boundary;
    // no scene-graph or backend object participates in identity or ownership.
    //
    // Each public mutation is published atomically. Stable source identities keep
    // handles unchanged across transform edits and cell moves, while RenderWorld
    // generations reject stale work after removal or a world-epoch reset.
    class ActiveCellProducer final
    {
    public:
        ActiveCellProducer(RenderWorld& world, RenderWorldPublisher& publisher)
            : mWorld(world)
            , mPublisher(publisher)
            , mObservedEpoch(world.epoch())
        {
        }

        [[nodiscard]] ActiveCellPublishResult addCell(const ActiveCellSource& source)
        {
            synchronizeEpoch();
            if (source.identity.empty())
                return failure(ActiveCellPublishStatus::InvalidSource);

            const auto [entry, inserted] = mCells.try_emplace(source.identity, ChunkHandle{});
            if (!inserted)
            {
                if (!mWorld.get(entry->second))
                    return failure(ActiveCellPublishStatus::PublishRejected);
                return { ActiveCellPublishStatus::AlreadyPresent, PublishStatus::Applied, entry->second, {}, {} };
            }

            const std::optional<ChunkHandle> handle = mWorld.reserveChunk();
            if (!handle)
            {
                mCells.erase(entry);
                return failure(ActiveCellPublishStatus::ReservationFailed);
            }

            ChunkRecord record;
            record.producerIdentity = source.identity;
            record.worldspaceIdentity = source.worldspaceIdentity;
            record.bounds = source.bounds;
            record.semanticFlags = source.semanticFlags;
            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), source.identity);
            const bool built = batch.sequence().valid() && batch.add(CreateChunk{ *handle, std::move(record) })
                && batch.seal();
            if (!built)
            {
                mWorld.cancel(*handle);
                mCells.erase(entry);
                return failure(batch.sequence().valid() ? ActiveCellPublishStatus::BatchBuildFailed
                                                        : ActiveCellPublishStatus::SequenceExhausted);
            }

            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
            {
                mWorld.cancel(*handle);
                mCells.erase(entry);
                return failure(ActiveCellPublishStatus::PublishRejected, published);
            }
            entry->second = *handle;
            return { ActiveCellPublishStatus::Applied, published, *handle, {}, {} };
        }

        [[nodiscard]] ActiveCellPublishResult removeCell(std::string_view identity)
        {
            synchronizeEpoch();
            const auto cell = mCells.find(identity);
            if (cell == mCells.end())
                return failure(ActiveCellPublishStatus::NotFound);

            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), std::string(identity));
            std::vector<std::string> members;
            for (const auto& [sourceIdentity, binding] : mInstances)
            {
                if (binding.cellIdentity == identity)
                {
                    members.push_back(sourceIdentity);
                    if (!batch.add(RetireInstance{ binding.handle }))
                        return failure(ActiveCellPublishStatus::BatchBuildFailed);
                }
            }
            std::vector<std::string> lights;
            for (const auto& [sourceIdentity, binding] : mLights)
            {
                if (binding.cellIdentity == identity)
                {
                    lights.push_back(sourceIdentity);
                    if (!batch.add(RetireLight{ binding.handle }))
                        return failure(ActiveCellPublishStatus::BatchBuildFailed);
                }
            }
            if (!batch.sequence().valid())
                return failure(ActiveCellPublishStatus::SequenceExhausted);
            if (!batch.add(RetireChunk{ cell->second }) || !batch.seal())
                return failure(ActiveCellPublishStatus::BatchBuildFailed);

            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
                return failure(ActiveCellPublishStatus::PublishRejected, published);
            for (const std::string& member : members)
                mInstances.erase(member);
            for (const std::string& light : lights)
                mLights.erase(light);
            const ChunkHandle removed = cell->second;
            mCells.erase(cell);
            return { ActiveCellPublishStatus::Applied, published, removed, {}, {} };
        }

        [[nodiscard]] ActiveCellPublishResult upsertStaticInstance(const StaticInstanceSource& source)
        {
            synchronizeEpoch();
            if (source.identity.empty() || source.cellIdentity.empty() || !source.model.valid())
                return failure(ActiveCellPublishStatus::InvalidSource);
            const auto cell = mCells.find(source.cellIdentity);
            if (cell == mCells.end() || !mWorld.get(cell->second))
                return failure(ActiveCellPublishStatus::MissingCell);
            if (!mWorld.get(source.model))
                return failure(ActiveCellPublishStatus::MissingModel);

            const auto existing = mInstances.find(source.identity);
            if (existing == mInstances.end())
                return createInstance(source, cell->second);
            return updateInstance(source, cell->second, existing);
        }

        [[nodiscard]] ActiveCellPublishResult upsertDynamicInstance(const DynamicInstanceSource& source)
        {
            synchronizeEpoch();
            if (source.identity.empty() || source.cellIdentity.empty() || !source.model.valid()
                || !source.skeleton.valid())
                return failure(ActiveCellPublishStatus::InvalidSource);
            const auto cell = mCells.find(source.cellIdentity);
            if (cell == mCells.end() || !mWorld.get(cell->second))
                return failure(ActiveCellPublishStatus::MissingCell);
            if (!mWorld.get(source.model))
                return failure(ActiveCellPublishStatus::MissingModel);
            if (!mWorld.get(source.skeleton))
                return failure(ActiveCellPublishStatus::MissingSkeleton);

            const auto existing = mInstances.find(source.identity);
            if (existing == mInstances.end())
                return createInstance(source, cell->second);
            return updateInstance(source, cell->second, existing);
        }

        [[nodiscard]] ActiveCellPublishResult removeStaticInstance(std::string_view identity)
        {
            return removeInstance(identity);
        }

        [[nodiscard]] ActiveCellPublishResult removeDynamicInstance(std::string_view identity)
        {
            return removeInstance(identity);
        }

        [[nodiscard]] ActiveCellPublishResult removeInstance(std::string_view identity)
        {
            synchronizeEpoch();
            const auto instance = mInstances.find(identity);
            if (instance == mInstances.end())
                return failure(ActiveCellPublishStatus::NotFound);

            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), std::string(identity));
            if (!batch.sequence().valid())
                return failure(ActiveCellPublishStatus::SequenceExhausted);
            if (!batch.add(RetireInstance{ instance->second.handle }) || !batch.seal())
                return failure(ActiveCellPublishStatus::BatchBuildFailed);
            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
                return failure(ActiveCellPublishStatus::PublishRejected, published);

            const InstanceHandle removed = instance->second.handle;
            mInstances.erase(instance);
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.instance = removed;
            return result;
        }

        [[nodiscard]] ActiveCellPublishResult upsertLight(const CellLightSource& source)
        {
            synchronizeEpoch();
            if (source.identity.empty() || source.cellIdentity.empty())
                return failure(ActiveCellPublishStatus::InvalidSource);
            const auto cell = mCells.find(source.cellIdentity);
            if (cell == mCells.end() || !mWorld.get(cell->second))
                return failure(ActiveCellPublishStatus::MissingCell);

            const auto existing = mLights.find(source.identity);
            if (existing == mLights.end())
                return createLight(source);
            return updateLight(source, existing);
        }

        [[nodiscard]] ActiveCellPublishResult removeLight(std::string_view identity)
        {
            synchronizeEpoch();
            const auto light = mLights.find(identity);
            if (light == mLights.end())
                return failure(ActiveCellPublishStatus::NotFound);

            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), std::string(identity));
            if (!batch.sequence().valid())
                return failure(ActiveCellPublishStatus::SequenceExhausted);
            if (!batch.add(RetireLight{ light->second.handle }) || !batch.seal())
                return failure(ActiveCellPublishStatus::BatchBuildFailed);
            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
                return failure(ActiveCellPublishStatus::PublishRejected, published);

            const LightHandle removed = light->second.handle;
            mLights.erase(light);
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.light = removed;
            return result;
        }

        [[nodiscard]] std::optional<ChunkHandle> findCell(std::string_view identity) const
        {
            const auto found = mCells.find(identity);
            return found == mCells.end() ? std::nullopt : std::optional<ChunkHandle>(found->second);
        }

        [[nodiscard]] std::optional<InstanceHandle> findInstance(std::string_view identity) const
        {
            const auto found = mInstances.find(identity);
            return found == mInstances.end() ? std::nullopt : std::optional<InstanceHandle>(found->second.handle);
        }

        [[nodiscard]] std::optional<LightHandle> findLight(std::string_view identity) const
        {
            const auto found = mLights.find(identity);
            return found == mLights.end() ? std::nullopt : std::optional<LightHandle>(found->second.handle);
        }

        [[nodiscard]] std::size_t cellCount() const noexcept { return mCells.size(); }
        [[nodiscard]] std::size_t instanceCount() const noexcept { return mInstances.size(); }
        [[nodiscard]] std::size_t lightCount() const noexcept { return mLights.size(); }

    private:
        struct InstanceBinding
        {
            InstanceHandle handle;
            std::string cellIdentity;
        };

        using InstanceMap = std::map<std::string, InstanceBinding, std::less<>>;

        struct LightBinding
        {
            LightHandle handle;
            std::string cellIdentity;
        };

        using LightMap = std::map<std::string, LightBinding, std::less<>>;

        [[nodiscard]] static ActiveCellPublishResult failure(
            ActiveCellPublishStatus status, PublishStatus worldStatus = PublishStatus::OperationRejected)
        {
            return { status, worldStatus, {}, {}, {} };
        }

        void synchronizeEpoch()
        {
            if (mObservedEpoch == mWorld.epoch())
                return;
            mObservedEpoch = mWorld.epoch();
            mCells.clear();
            mInstances.clear();
            mLights.clear();
        }

        [[nodiscard]] UpdateSequence nextSequence() const noexcept
        {
            // One publisher may be shared by asset, cell, terrain, and dynamic
            // producers. Derive the next sequence from that common commit point
            // so independently implemented producers cannot collide.
            return mPublisher.nextSequence();
        }

        [[nodiscard]] static InstanceRecord makeRecord(
            const StaticInstanceSource& source, ChunkHandle chunk, ResourceRevision revision)
        {
            InstanceRecord record;
            record.revision = revision;
            record.chunk = chunk;
            record.model = source.model;
            record.transform = source.transform;
            record.localBounds = source.localBounds;
            record.lod = source.lod;
            record.semanticFlags = source.semanticFlags;
            record.lightingEnabled = source.lightingEnabled;
            return record;
        }

        [[nodiscard]] static InstanceRecord makeRecord(
            const DynamicInstanceSource& source, ChunkHandle chunk, ResourceRevision revision)
        {
            InstanceRecord record;
            record.revision = revision;
            record.chunk = chunk;
            record.model = source.model;
            record.skeleton = source.skeleton;
            record.transform = source.transform;
            record.localBounds = source.localBounds;
            record.lod = source.lod;
            record.semanticFlags = source.semanticFlags;
            record.lightingEnabled = source.lightingEnabled;
            return record;
        }

        template <class Source>
        [[nodiscard]] ActiveCellPublishResult createInstance(const Source& source, ChunkHandle chunk)
        {
            const auto [entry, inserted]
                = mInstances.try_emplace(source.identity, InstanceBinding{ {}, source.cellIdentity });
            if (!inserted)
                return failure(ActiveCellPublishStatus::PublishRejected);
            const std::optional<InstanceHandle> handle = mWorld.reserveInstance();
            if (!handle)
            {
                mInstances.erase(entry);
                return failure(ActiveCellPublishStatus::ReservationFailed);
            }

            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), source.identity);
            const bool built = batch.sequence().valid()
                && batch.add(CreateInstance{ *handle, makeRecord(source, chunk, InitialResourceRevision) })
                && batch.seal();
            if (!built)
            {
                mWorld.cancel(*handle);
                mInstances.erase(entry);
                return failure(batch.sequence().valid() ? ActiveCellPublishStatus::BatchBuildFailed
                                                        : ActiveCellPublishStatus::SequenceExhausted);
            }
            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
            {
                mWorld.cancel(*handle);
                mInstances.erase(entry);
                return failure(ActiveCellPublishStatus::PublishRejected, published);
            }
            entry->second.handle = *handle;
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.instance = *handle;
            return result;
        }

        template <class Source>
        [[nodiscard]] ActiveCellPublishResult updateInstance(
            const Source& source, ChunkHandle chunk, InstanceMap::iterator existing)
        {
            const InstanceRecord* current = mWorld.get(existing->second.handle);
            if (!current)
                return failure(ActiveCellPublishStatus::PublishRejected);

            const bool moving = existing->second.cellIdentity != source.cellIdentity;
            std::optional<ResourceRevision> revision = advanceMonotonic(current->revision);
            if (moving && revision)
                revision = advanceMonotonic(*revision);
            if (!revision)
                return failure(ActiveCellPublishStatus::ResourceRevisionExhausted);

            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), source.identity);
            if (!batch.sequence().valid())
                return failure(ActiveCellPublishStatus::SequenceExhausted);
            if (moving && !batch.add(ReparentInstance{ existing->second.handle, chunk }))
                return failure(ActiveCellPublishStatus::BatchBuildFailed);
            if (!batch.add(UpdateInstance{ existing->second.handle, makeRecord(source, chunk, *revision) })
                || !batch.seal())
                return failure(ActiveCellPublishStatus::BatchBuildFailed);

            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
                return failure(ActiveCellPublishStatus::PublishRejected, published);
            existing->second.cellIdentity = source.cellIdentity;
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.instance = existing->second.handle;
            return result;
        }

        [[nodiscard]] ActiveCellPublishResult createLight(const CellLightSource& source)
        {
            const auto [entry, inserted]
                = mLights.try_emplace(source.identity, LightBinding{ {}, source.cellIdentity });
            if (!inserted)
                return failure(ActiveCellPublishStatus::PublishRejected);
            const std::optional<LightHandle> handle = mWorld.reserveLight();
            if (!handle)
            {
                mLights.erase(entry);
                return failure(ActiveCellPublishStatus::ReservationFailed);
            }

            LightRecord record = source.light;
            record.revision = InitialResourceRevision;
            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), source.identity);
            const bool built
                = batch.sequence().valid() && batch.add(CreateLight{ *handle, std::move(record) }) && batch.seal();
            if (!built)
            {
                mWorld.cancel(*handle);
                mLights.erase(entry);
                return failure(batch.sequence().valid() ? ActiveCellPublishStatus::BatchBuildFailed
                                                        : ActiveCellPublishStatus::SequenceExhausted);
            }
            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
            {
                mWorld.cancel(*handle);
                mLights.erase(entry);
                return failure(ActiveCellPublishStatus::PublishRejected, published);
            }
            entry->second.handle = *handle;
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.light = *handle;
            return result;
        }

        [[nodiscard]] ActiveCellPublishResult updateLight(
            const CellLightSource& source, LightMap::iterator existing)
        {
            const LightRecord* current = mWorld.get(existing->second.handle);
            if (!current)
                return failure(ActiveCellPublishStatus::PublishRejected);
            const std::optional<ResourceRevision> revision = advanceMonotonic(current->revision);
            if (!revision)
                return failure(ActiveCellPublishStatus::ResourceRevisionExhausted);

            LightRecord record = source.light;
            record.revision = *revision;
            RenderWorldUpdateBatch batch(mWorld.epoch(), nextSequence(), source.identity);
            if (!batch.sequence().valid())
                return failure(ActiveCellPublishStatus::SequenceExhausted);
            if (!batch.add(UpdateLight{ existing->second.handle, std::move(record) }) || !batch.seal())
                return failure(ActiveCellPublishStatus::BatchBuildFailed);
            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
                return failure(ActiveCellPublishStatus::PublishRejected, published);

            existing->second.cellIdentity = source.cellIdentity;
            ActiveCellPublishResult result{ ActiveCellPublishStatus::Applied, published, {}, {}, {} };
            result.light = existing->second.handle;
            return result;
        }

        RenderWorld& mWorld;
        RenderWorldPublisher& mPublisher;
        WorldEpoch mObservedEpoch;
        std::map<std::string, ChunkHandle, std::less<>> mCells;
        InstanceMap mInstances;
        LightMap mLights;
    };
}

#endif
