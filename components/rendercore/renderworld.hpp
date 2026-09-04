#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H

#include "handles.hpp"
#include "math.hpp"
#include "resources.hpp"
#include "slottable.hpp"

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

        bool commit(MeshHandle handle, MeshRecord record) { return commitRecord(mMeshes, handle, std::move(record)); }
        bool commit(MaterialHandle handle, MaterialRecord record)
        {
            return commitRecord(mMaterials, handle, std::move(record));
        }
        bool commit(TextureHandle handle, TextureRecord record)
        {
            return commitRecord(mTextures, handle, std::move(record));
        }
        bool commit(SkeletonHandle handle, SkeletonRecord record)
        {
            return commitRecord(mSkeletons, handle, std::move(record));
        }
        bool commit(ChunkHandle handle, ChunkRecord record) { return commitRecord(mChunks, handle, std::move(record)); }
        bool commit(LightHandle handle, LightRecord record) { return commitRecord(mLights, handle, std::move(record)); }

        bool commit(InstanceHandle handle, InstanceRecord record)
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
            return commitRecord(mInstances, handle, std::move(record));
        }

        bool cancel(MeshHandle handle) noexcept { return mMeshes.cancel(handle); }
        bool cancel(MaterialHandle handle) noexcept { return mMaterials.cancel(handle); }
        bool cancel(TextureHandle handle) noexcept { return mTextures.cancel(handle); }
        bool cancel(SkeletonHandle handle) noexcept { return mSkeletons.cancel(handle); }
        bool cancel(InstanceHandle handle) noexcept { return mInstances.cancel(handle); }
        bool cancel(ChunkHandle handle) noexcept { return mChunks.cancel(handle); }
        bool cancel(LightHandle handle) noexcept { return mLights.cancel(handle); }

        bool retire(MeshHandle handle) noexcept { return retireRecord(mMeshes, handle); }
        bool retire(MaterialHandle handle) noexcept { return retireRecord(mMaterials, handle); }
        bool retire(TextureHandle handle) noexcept { return retireRecord(mTextures, handle); }
        bool retire(SkeletonHandle handle) noexcept { return retireRecord(mSkeletons, handle); }
        bool retire(InstanceHandle handle) noexcept { return retireRecord(mInstances, handle); }
        bool retire(ChunkHandle handle) noexcept { return retireRecord(mChunks, handle); }
        bool retire(LightHandle handle) noexcept { return retireRecord(mLights, handle); }

        [[nodiscard]] const MeshRecord* get(MeshHandle handle) const noexcept { return mMeshes.get(handle); }
        [[nodiscard]] const MaterialRecord* get(MaterialHandle handle) const noexcept { return mMaterials.get(handle); }
        [[nodiscard]] const TextureRecord* get(TextureHandle handle) const noexcept { return mTextures.get(handle); }
        [[nodiscard]] const SkeletonRecord* get(SkeletonHandle handle) const noexcept { return mSkeletons.get(handle); }
        [[nodiscard]] const InstanceRecord* get(InstanceHandle handle) const noexcept { return mInstances.get(handle); }
        [[nodiscard]] const ChunkRecord* get(ChunkHandle handle) const noexcept { return mChunks.get(handle); }
        [[nodiscard]] const LightRecord* get(LightHandle handle) const noexcept { return mLights.get(handle); }

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
        template <class Table, class Handle, class Record>
        bool commitRecord(Table& table, Handle handle, Record record)
        {
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision || !table.commit(handle, std::move(record)))
                return false;
            mRevision = *nextRevision;
            return true;
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
