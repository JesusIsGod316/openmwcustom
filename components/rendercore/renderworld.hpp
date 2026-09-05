#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H

#include "handles.hpp"
#include "math.hpp"
#include "resources.hpp"
#include "slottable.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace RenderCore
{
    struct MeshRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
        std::uint32_t surfaceCount = 0;
        bool skinned = false;
        bool morphed = false;
    };

    struct MaterialRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
    };

    struct TextureRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
    };

    struct SkeletonRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
    };

    enum class InstanceSemanticFlag : std::uint64_t
    {
        OrdinaryWorld = 1ull << 0,
        OwnerBody = 1ull << 1,
        OwnerHead = 1ull << 2,
        ShadowCaster = 1ull << 3,
        ReflectionEligible = 1ull << 4,
        RefractionEligible = 1ull << 5,
        MapEligible = 1ull << 6,
        PreviewEligible = 1ull << 7,
    };

    [[nodiscard]] constexpr std::uint64_t semanticFlag(InstanceSemanticFlag flag) noexcept
    {
        return static_cast<std::uint64_t>(flag);
    }

    struct InstanceRecord
    {
        // Canonical semantic ownership. ChunkRecord::members is a derived ordered
        // reverse index maintained only by RenderWorld publication operations.
        std::optional<ChunkHandle> chunk;
        MeshHandle mesh;
        std::vector<MaterialHandle> materials;
        std::optional<SkeletonHandle> skeleton;
        WorldTransform transform;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster)
            | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    struct ChunkRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string producerIdentity;
        // Derived ordered index for individually addressable instances. Producers
        // never author this independently from InstanceRecord::chunk.
        std::vector<InstanceHandle> members;
    };

    struct LightRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        WorldPosition position{ 0.0, 0.0, 0.0 };
        Color diffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
        float constantAttenuation = 1.0f;
        float linearAttenuation = 0.0f;
        float quadraticAttenuation = 0.0f;
        float effectiveRadius = 0.0f;
        bool enabled = true;
    };

    class RenderWorld
    {
    public:
        using MeshTable = SlotTable<MeshHandle, MeshRecord>;
        using MaterialTable = SlotTable<MaterialHandle, MaterialRecord>;
        using TextureTable = SlotTable<TextureHandle, TextureRecord>;
        using SkeletonTable = SlotTable<SkeletonHandle, SkeletonRecord>;
        using InstanceTable = SlotTable<InstanceHandle, InstanceRecord>;
        using ChunkTable = SlotTable<ChunkHandle, ChunkRecord>;
        using LightTable = SlotTable<LightHandle, LightRecord>;

        [[nodiscard]] WorldEpoch epoch() const noexcept { return mEpoch; }
        [[nodiscard]] RenderWorldRevision revision() const noexcept { return mRevision; }

        [[nodiscard]] std::optional<MeshHandle> reserveMesh() { return mMeshes.reserve(); }
        [[nodiscard]] std::optional<MaterialHandle> reserveMaterial() { return mMaterials.reserve(); }
        [[nodiscard]] std::optional<TextureHandle> reserveTexture() { return mTextures.reserve(); }
        [[nodiscard]] std::optional<SkeletonHandle> reserveSkeleton() { return mSkeletons.reserve(); }
        [[nodiscard]] std::optional<InstanceHandle> reserveInstance() { return mInstances.reserve(); }
        [[nodiscard]] std::optional<ChunkHandle> reserveChunk() { return mChunks.reserve(); }
        [[nodiscard]] std::optional<LightHandle> reserveLight() { return mLights.reserve(); }

        bool commit(MeshHandle handle, MeshRecord record)
        {
            return record.revision.valid() && commitRecord(mMeshes, handle, std::move(record));
        }
        bool commit(MaterialHandle handle, MaterialRecord record)
        {
            return record.revision.valid() && commitRecord(mMaterials, handle, std::move(record));
        }
        bool commit(TextureHandle handle, TextureRecord record)
        {
            return record.revision.valid() && commitRecord(mTextures, handle, std::move(record));
        }
        bool commit(SkeletonHandle handle, SkeletonRecord record)
        {
            return record.revision.valid() && commitRecord(mSkeletons, handle, std::move(record));
        }
        bool commit(ChunkHandle handle, ChunkRecord record)
        {
            // Individual membership is derived from instance publication. Accepting
            // caller-authored members here would create a second semantic write path.
            if (!record.revision.valid() || !record.members.empty())
                return false;
            return commitRecord(mChunks, handle, std::move(record));
        }
        bool commit(LightHandle handle, LightRecord record)
        {
            return record.revision.valid() && commitRecord(mLights, handle, std::move(record));
        }

        bool commit(InstanceHandle handle, InstanceRecord record)
        {
            if (!mInstances.isReserved(handle) || !validateInstanceReferences(record))
                return false;

            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision)
                return false;

            ChunkRecord* chunk = record.chunk ? mChunks.get(*record.chunk) : nullptr;
            if (chunk && containsMember(*chunk, handle))
                return false;

            // Publish the derived reverse index first. push_back can allocate and
            // throw; if it does, no instance state has changed. If instance commit
            // later throws/fails, roll the index back before propagating/returning.
            if (chunk)
                chunk->members.push_back(handle);

            try
            {
                if (!mInstances.commit(handle, std::move(record)))
                {
                    if (chunk)
                        chunk->members.pop_back();
                    return false;
                }
            }
            catch (...)
            {
                if (chunk)
                    chunk->members.pop_back();
                throw;
            }

            mRevision = *nextRevision;
            return true;
        }

        // Same-generation payload replacement. Versioned records must advance
        // ResourceRevision. Chunk membership itself is not writable through this
        // generic update API; relationship changes use reparentInstance().
        bool update(MeshHandle handle, MeshRecord record)
        {
            return updateVersionedRecord(mMeshes, handle, std::move(record));
        }
        bool update(MaterialHandle handle, MaterialRecord record)
        {
            return updateVersionedRecord(mMaterials, handle, std::move(record));
        }
        bool update(TextureHandle handle, TextureRecord record)
        {
            return updateVersionedRecord(mTextures, handle, std::move(record));
        }
        bool update(SkeletonHandle handle, SkeletonRecord record)
        {
            return updateVersionedRecord(mSkeletons, handle, std::move(record));
        }
        bool update(LightHandle handle, LightRecord record)
        {
            return updateVersionedRecord(mLights, handle, std::move(record));
        }

        bool update(ChunkHandle handle, ChunkRecord record)
        {
            const ChunkRecord* current = mChunks.get(handle);
            if (!current || record.members != current->members)
                return false;
            return updateVersionedRecord(mChunks, handle, std::move(record));
        }

        bool update(InstanceHandle handle, InstanceRecord record)
        {
            const InstanceRecord* current = mInstances.get(handle);
            if (!current || record.chunk != current->chunk || !validateInstanceReferences(record))
                return false;
            if (record.chunk && !chunkContainsExactlyOnce(*record.chunk, handle))
                return false;
            return updateRecord(mInstances, handle, std::move(record));
        }

        // Move one live individually addressable instance between logical chunks.
        // InstanceRecord::chunk is the semantic source of truth and members is
        // updated in the same published RenderWorld revision.
        bool reparentInstance(InstanceHandle handle, std::optional<ChunkHandle> newChunk)
        {
            InstanceRecord* instance = mInstances.get(handle);
            if (!instance)
                return false;
            if (newChunk && !mChunks.contains(*newChunk))
                return false;

            ChunkRecord* oldRecord = instance->chunk ? mChunks.get(*instance->chunk) : nullptr;
            ChunkRecord* newRecord = newChunk ? mChunks.get(*newChunk) : nullptr;
            if (instance->chunk && (!oldRecord || !containsExactlyOnce(*oldRecord, handle)))
                return false;

            if (instance->chunk == newChunk)
                return !newRecord || containsExactlyOnce(*newRecord, handle);
            if (newRecord && containsMember(*newRecord, handle))
                return false;

            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision)
                return false;

            // New membership is the only step that can allocate/throw. Perform it
            // before no-throw erasure/handle assignment so failure leaves the old
            // published relationship untouched.
            if (newRecord)
                newRecord->members.push_back(handle);
            if (oldRecord)
            {
                const auto oldMember = std::find(oldRecord->members.begin(), oldRecord->members.end(), handle);
                oldRecord->members.erase(oldMember);
            }
            instance->chunk = newChunk;
            mRevision = *nextRevision;
            return true;
        }

        bool cancel(MeshHandle handle) noexcept { return mMeshes.cancel(handle); }
        bool cancel(MaterialHandle handle) noexcept { return mMaterials.cancel(handle); }
        bool cancel(TextureHandle handle) noexcept { return mTextures.cancel(handle); }
        bool cancel(SkeletonHandle handle) noexcept { return mSkeletons.cancel(handle); }
        bool cancel(InstanceHandle handle) noexcept { return mInstances.cancel(handle); }
        bool cancel(ChunkHandle handle) noexcept { return mChunks.cancel(handle); }
        bool cancel(LightHandle handle) noexcept { return mLights.cancel(handle); }

        // Logical retirement fails closed while live dependents still reference
        // the target. Backend residency retirement remains independent.
        bool retire(MeshHandle handle) noexcept
        {
            return !meshReferenced(handle) && retireRecord(mMeshes, handle);
        }
        bool retire(MaterialHandle handle) noexcept
        {
            return !materialReferenced(handle) && retireRecord(mMaterials, handle);
        }
        bool retire(TextureHandle handle) noexcept { return retireRecord(mTextures, handle); }
        bool retire(SkeletonHandle handle) noexcept
        {
            return !skeletonReferenced(handle) && retireRecord(mSkeletons, handle);
        }

        bool retire(InstanceHandle handle) noexcept
        {
            InstanceRecord* instance = mInstances.get(handle);
            if (!instance)
                return false;

            ChunkRecord* chunk = instance->chunk ? mChunks.get(*instance->chunk) : nullptr;
            if (instance->chunk && (!chunk || !containsExactlyOnce(*chunk, handle)))
                return false;

            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision || !mInstances.retire(handle))
                return false;

            if (chunk)
            {
                const auto member = std::find(chunk->members.begin(), chunk->members.end(), handle);
                chunk->members.erase(member);
            }
            mRevision = *nextRevision;
            return true;
        }

        bool retire(ChunkHandle handle) noexcept
        {
            const ChunkRecord* chunk = mChunks.get(handle);
            if (!chunk || !chunk->members.empty() || chunkReferenced(handle))
                return false;
            return retireRecord(mChunks, handle);
        }
        bool retire(LightHandle handle) noexcept { return retireRecord(mLights, handle); }

        [[nodiscard]] const MeshRecord* get(MeshHandle handle) const noexcept { return mMeshes.get(handle); }
        [[nodiscard]] const MaterialRecord* get(MaterialHandle handle) const noexcept { return mMaterials.get(handle); }
        [[nodiscard]] const TextureRecord* get(TextureHandle handle) const noexcept { return mTextures.get(handle); }
        [[nodiscard]] const SkeletonRecord* get(SkeletonHandle handle) const noexcept { return mSkeletons.get(handle); }
        [[nodiscard]] const InstanceRecord* get(InstanceHandle handle) const noexcept { return mInstances.get(handle); }
        [[nodiscard]] const ChunkRecord* get(ChunkHandle handle) const noexcept { return mChunks.get(handle); }
        [[nodiscard]] const LightRecord* get(LightHandle handle) const noexcept { return mLights.get(handle); }

        // Expensive correctness audit intended for publication/checkpoint tests,
        // not per-draw traversal. Public mutation methods should preserve it after
        // every successful operation.
        [[nodiscard]] bool valid() const noexcept
        {
            bool result = true;

            mMeshes.forEachLive([&](MeshHandle, const MeshRecord& record) {
                if (!record.revision.valid())
                    result = false;
            });
            mMaterials.forEachLive([&](MaterialHandle, const MaterialRecord& record) {
                if (!record.revision.valid())
                    result = false;
            });
            mTextures.forEachLive([&](TextureHandle, const TextureRecord& record) {
                if (!record.revision.valid())
                    result = false;
            });
            mSkeletons.forEachLive([&](SkeletonHandle, const SkeletonRecord& record) {
                if (!record.revision.valid())
                    result = false;
            });
            mLights.forEachLive([&](LightHandle, const LightRecord& record) {
                if (!record.revision.valid())
                    result = false;
            });

            mChunks.forEachLive([&](ChunkHandle chunkHandle, const ChunkRecord& chunk) {
                if (!chunk.revision.valid())
                {
                    result = false;
                    return;
                }
                for (std::size_t i = 0; i < chunk.members.size(); ++i)
                {
                    const InstanceHandle member = chunk.members[i];
                    const InstanceRecord* instance = mInstances.get(member);
                    if (!instance || !instance->chunk || *instance->chunk != chunkHandle)
                        result = false;
                    for (std::size_t j = i + 1; j < chunk.members.size(); ++j)
                    {
                        if (member == chunk.members[j])
                            result = false;
                    }
                }
            });

            mInstances.forEachLive([&](InstanceHandle handle, const InstanceRecord& instance) {
                if (!validateInstanceReferences(instance))
                {
                    result = false;
                    return;
                }
                if (instance.chunk && !chunkContainsExactlyOnce(*instance.chunk, handle))
                    result = false;
            });

            return result;
        }

        // Destructive semantic reset. Slot generations advance so stale handles
        // fail closed even before the new worldEpoch is checked by batch logic.
        bool reset() noexcept
        {
            const auto nextEpoch = advanceMonotonic(mEpoch);
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextEpoch || !nextRevision)
                return false;

            mMeshes.retireAll();
            mMaterials.retireAll();
            mTextures.retireAll();
            mSkeletons.retireAll();
            mInstances.retireAll();
            mChunks.retireAll();
            mLights.retireAll();

            mEpoch = *nextEpoch;
            mRevision = *nextRevision;
            return true;
        }

    private:
        [[nodiscard]] bool validateInstanceReferences(const InstanceRecord& record) const noexcept
        {
            if (!mMeshes.contains(record.mesh))
                return false;
            if (record.chunk && !mChunks.contains(*record.chunk))
                return false;
            if (record.skeleton && !mSkeletons.contains(*record.skeleton))
                return false;
            for (const MaterialHandle material : record.materials)
            {
                if (!mMaterials.contains(material))
                    return false;
            }
            return true;
        }

        [[nodiscard]] static bool containsMember(const ChunkRecord& chunk, InstanceHandle handle) noexcept
        {
            return std::find(chunk.members.begin(), chunk.members.end(), handle) != chunk.members.end();
        }

        [[nodiscard]] static bool containsExactlyOnce(const ChunkRecord& chunk, InstanceHandle handle) noexcept
        {
            return std::count(chunk.members.begin(), chunk.members.end(), handle) == 1;
        }

        [[nodiscard]] bool chunkContainsExactlyOnce(ChunkHandle chunk, InstanceHandle instance) const noexcept
        {
            const ChunkRecord* record = mChunks.get(chunk);
            return record && containsExactlyOnce(*record, instance);
        }

        [[nodiscard]] bool meshReferenced(MeshHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (record.mesh == handle)
                    referenced = true;
            });
            return referenced;
        }

        [[nodiscard]] bool materialReferenced(MaterialHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (std::find(record.materials.begin(), record.materials.end(), handle) != record.materials.end())
                    referenced = true;
            });
            return referenced;
        }

        [[nodiscard]] bool skeletonReferenced(SkeletonHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (record.skeleton && *record.skeleton == handle)
                    referenced = true;
            });
            return referenced;
        }

        [[nodiscard]] bool chunkReferenced(ChunkHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (record.chunk && *record.chunk == handle)
                    referenced = true;
            });
            return referenced;
        }

        template <class Table, class Handle, class Record>
        bool commitRecord(Table& table, Handle handle, Record record)
        {
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision || !table.commit(handle, std::move(record)))
                return false;
            mRevision = *nextRevision;
            return true;
        }

        template <class Table, class Handle, class Record>
        bool updateRecord(Table& table, Handle handle, Record record)
        {
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision || !table.update(handle, std::move(record)))
                return false;
            mRevision = *nextRevision;
            return true;
        }

        template <class Table, class Handle, class Record>
        bool updateVersionedRecord(Table& table, Handle handle, Record record)
        {
            const Record* current = table.get(handle);
            if (!current || !record.revision.valid() || record.revision <= current->revision)
                return false;
            return updateRecord(table, handle, std::move(record));
        }

        template <class Table, class Handle>
        bool retireRecord(Table& table, Handle handle) noexcept
        {
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision || !table.retire(handle))
                return false;
            mRevision = *nextRevision;
            return true;
        }

        WorldEpoch mEpoch = InitialWorldEpoch;
        RenderWorldRevision mRevision = InitialRenderWorldRevision;
        MeshTable mMeshes;
        MaterialTable mMaterials;
        TextureTable mTextures;
        SkeletonTable mSkeletons;
        InstanceTable mInstances;
        ChunkTable mChunks;
        LightTable mLights;
    };
}

#endif
