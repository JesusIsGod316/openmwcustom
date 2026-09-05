#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERWORLD_H

#include "records.hpp"
#include "slottable.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace RenderCore
{
    class RenderWorld
    {
    public:
        using MeshTable = SlotTable<MeshHandle, MeshRecord>;
        using ModelTable = SlotTable<ModelHandle, ModelRecord>;
        using MaterialTable = SlotTable<MaterialHandle, MaterialRecord>;
        using TextureTable = SlotTable<TextureHandle, TextureRecord>;
        using SkeletonTable = SlotTable<SkeletonHandle, SkeletonRecord>;
        using InstanceTable = SlotTable<InstanceHandle, InstanceRecord>;
        using ChunkTable = SlotTable<ChunkHandle, ChunkRecord>;
        using LightTable = SlotTable<LightHandle, LightRecord>;

        [[nodiscard]] WorldEpoch epoch() const noexcept { return mEpoch; }
        [[nodiscard]] RenderWorldRevision revision() const noexcept { return mRevision; }

        [[nodiscard]] std::optional<MeshHandle> reserveMesh() { return mMeshes.reserve(); }
        [[nodiscard]] std::optional<ModelHandle> reserveModel() { return mModels.reserve(); }
        [[nodiscard]] std::optional<MaterialHandle> reserveMaterial() { return mMaterials.reserve(); }
        [[nodiscard]] std::optional<TextureHandle> reserveTexture() { return mTextures.reserve(); }
        [[nodiscard]] std::optional<SkeletonHandle> reserveSkeleton() { return mSkeletons.reserve(); }
        [[nodiscard]] std::optional<InstanceHandle> reserveInstance() { return mInstances.reserve(); }
        [[nodiscard]] std::optional<ChunkHandle> reserveChunk() { return mChunks.reserve(); }
        [[nodiscard]] std::optional<LightHandle> reserveLight() { return mLights.reserve(); }

        bool commit(MeshHandle handle, MeshRecord record)
        {
            return validateMeshRecord(record) && commitRecord(mMeshes, handle, std::move(record));
        }

        bool commit(ModelHandle handle, ModelRecord record)
        {
            return validateModelRecord(record) && commitRecord(mModels, handle, std::move(record));
        }

        bool commit(MaterialHandle handle, MaterialRecord record)
        {
            return validateMaterialRecord(record) && commitRecord(mMaterials, handle, std::move(record));
        }

        bool commit(TextureHandle handle, TextureRecord record)
        {
            return validateTextureRecord(record) && commitRecord(mTextures, handle, std::move(record));
        }

        bool commit(SkeletonHandle handle, SkeletonRecord record)
        {
            return validateSkeletonRecord(record) && commitRecord(mSkeletons, handle, std::move(record));
        }

        bool commit(ChunkHandle handle, ChunkRecord record)
        {
            if (!validateChunkRecord(record) || !record.members.empty())
                return false;
            return commitRecord(mChunks, handle, std::move(record));
        }

        bool commit(LightHandle handle, LightRecord record)
        {
            return validateLightRecord(record) && commitRecord(mLights, handle, std::move(record));
        }

        bool commit(InstanceHandle handle, InstanceRecord record)
        {
            if (!mInstances.isReserved(handle) || !validateInstanceReferences(handle, record))
                return false;

            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextRevision)
                return false;

            ChunkRecord* chunk = record.chunk ? mChunks.get(*record.chunk) : nullptr;
            if (chunk && containsMember(*chunk, handle))
                return false;

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

        bool update(MeshHandle handle, MeshRecord record)
        {
            return validateMeshRecord(record) && updateVersionedRecord(mMeshes, handle, std::move(record));
        }

        bool update(ModelHandle handle, ModelRecord record)
        {
            return validateModelRecord(record) && updateVersionedRecord(mModels, handle, std::move(record));
        }

        bool update(MaterialHandle handle, MaterialRecord record)
        {
            return validateMaterialRecord(record) && updateVersionedRecord(mMaterials, handle, std::move(record));
        }

        bool update(TextureHandle handle, TextureRecord record)
        {
            return validateTextureRecord(record) && updateVersionedRecord(mTextures, handle, std::move(record));
        }

        bool update(SkeletonHandle handle, SkeletonRecord record)
        {
            return validateSkeletonRecord(record) && updateVersionedRecord(mSkeletons, handle, std::move(record));
        }

        bool update(LightHandle handle, LightRecord record)
        {
            return validateLightRecord(record) && updateVersionedRecord(mLights, handle, std::move(record));
        }

        bool update(ChunkHandle handle, ChunkRecord record)
        {
            const ChunkRecord* current = mChunks.get(handle);
            if (!current || record.members != current->members || !validateChunkRecord(record))
                return false;
            return updateVersionedRecord(mChunks, handle, std::move(record));
        }

        bool update(InstanceHandle handle, InstanceRecord record)
        {
            const InstanceRecord* current = mInstances.get(handle);
            if (!current || record.chunk != current->chunk || !validateInstanceReferences(handle, record))
                return false;
            if (record.chunk && !chunkContainsExactlyOnce(*record.chunk, handle))
                return false;
            return updateRecord(mInstances, handle, std::move(record));
        }

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
        bool cancel(ModelHandle handle) noexcept { return mModels.cancel(handle); }
        bool cancel(MaterialHandle handle) noexcept { return mMaterials.cancel(handle); }
        bool cancel(TextureHandle handle) noexcept { return mTextures.cancel(handle); }
        bool cancel(SkeletonHandle handle) noexcept { return mSkeletons.cancel(handle); }
        bool cancel(InstanceHandle handle) noexcept { return mInstances.cancel(handle); }
        bool cancel(ChunkHandle handle) noexcept { return mChunks.cancel(handle); }
        bool cancel(LightHandle handle) noexcept { return mLights.cancel(handle); }

        bool retire(MeshHandle handle) noexcept
        {
            return !meshReferenced(handle) && retireRecord(mMeshes, handle);
        }

        bool retire(ModelHandle handle) noexcept
        {
            return !modelReferenced(handle) && retireRecord(mModels, handle);
        }

        bool retire(MaterialHandle handle) noexcept
        {
            return !materialReferenced(handle) && retireRecord(mMaterials, handle);
        }

        bool retire(TextureHandle handle) noexcept
        {
            return !textureReferenced(handle) && retireRecord(mTextures, handle);
        }

        bool retire(SkeletonHandle handle) noexcept
        {
            return !skeletonReferenced(handle) && retireRecord(mSkeletons, handle);
        }

        bool retire(InstanceHandle handle) noexcept
        {
            InstanceRecord* instance = mInstances.get(handle);
            if (!instance || instanceReferenced(handle))
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
        [[nodiscard]] const ModelRecord* get(ModelHandle handle) const noexcept { return mModels.get(handle); }
        [[nodiscard]] const MaterialRecord* get(MaterialHandle handle) const noexcept { return mMaterials.get(handle); }
        [[nodiscard]] const TextureRecord* get(TextureHandle handle) const noexcept { return mTextures.get(handle); }
        [[nodiscard]] const SkeletonRecord* get(SkeletonHandle handle) const noexcept { return mSkeletons.get(handle); }
        [[nodiscard]] const InstanceRecord* get(InstanceHandle handle) const noexcept { return mInstances.get(handle); }
        [[nodiscard]] const ChunkRecord* get(ChunkHandle handle) const noexcept { return mChunks.get(handle); }
        [[nodiscard]] const LightRecord* get(LightHandle handle) const noexcept { return mLights.get(handle); }

        [[nodiscard]] bool valid() const noexcept
        {
            bool result = true;

            mMeshes.forEachLive([&](MeshHandle, const MeshRecord& record) {
                if (!validateMeshRecord(record))
                    result = false;
            });
            mModels.forEachLive([&](ModelHandle, const ModelRecord& record) {
                if (!validateModelRecord(record))
                    result = false;
            });
            mMaterials.forEachLive([&](MaterialHandle, const MaterialRecord& record) {
                if (!validateMaterialRecord(record))
                    result = false;
            });
            mTextures.forEachLive([&](TextureHandle, const TextureRecord& record) {
                if (!validateTextureRecord(record))
                    result = false;
            });
            mSkeletons.forEachLive([&](SkeletonHandle, const SkeletonRecord& record) {
                if (!validateSkeletonRecord(record))
                    result = false;
            });
            mLights.forEachLive([&](LightHandle, const LightRecord& record) {
                if (!validateLightRecord(record))
                    result = false;
            });

            mChunks.forEachLive([&](ChunkHandle chunkHandle, const ChunkRecord& chunk) {
                if (!validateChunkRecord(chunk))
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
                if (!validateInstanceReferences(handle, instance))
                {
                    result = false;
                    return;
                }
                if (instance.chunk && !chunkContainsExactlyOnce(*instance.chunk, handle))
                    result = false;
            });

            return result;
        }

        bool reset() noexcept
        {
            const auto nextEpoch = advanceMonotonic(mEpoch);
            const auto nextRevision = advanceMonotonic(mRevision);
            if (!nextEpoch || !nextRevision)
                return false;

            mMeshes.retireAll();
            mModels.retireAll();
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
        [[nodiscard]] bool validateMeshRecord(const MeshRecord& record) const noexcept
        {
            if (!record.revision.valid())
                return false;
            if (record.payload)
            {
                if (!validMeshPayload(*record.payload))
                    return false;
                if (record.surfaceCount != record.payload->surfaces.size())
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool validateModelRecord(const ModelRecord& record) const noexcept
        {
            if (!record.revision.valid() || !record.payload || !validModelPayloadStructure(*record.payload))
                return false;

            for (const ModelNodeRecord& node : record.payload->nodes)
            {
                if (node.mesh && !mMeshes.contains(*node.mesh))
                    return false;
                for (const MaterialHandle material : node.materials)
                {
                    if (!mMaterials.contains(material))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool validateMaterialRecord(const MaterialRecord& record) const noexcept
        {
            if (!record.revision.valid() || !semantic_detail::finite(record.diffuse)
                || !semantic_detail::finite(record.ambient) || !semantic_detail::finite(record.specular)
                || !semantic_detail::finite(record.emission) || !semantic_detail::finite(record.environmentMapColor)
                || !std::isfinite(record.shininess) || !std::isfinite(record.emissiveMultiplier)
                || !std::isfinite(record.specularStrength) || !std::isfinite(record.environmentMapStrength)
                || !std::isfinite(record.alpha) || !std::isfinite(record.alphaCutoff))
                return false;
            for (const TextureBinding& binding : record.textures)
            {
                if (!mTextures.contains(binding.texture) || !std::isfinite(binding.sampler.maxAnisotropy)
                    || binding.sampler.maxAnisotropy < 0.0f || !semantic_detail::finite(binding.transform.offset)
                    || !semantic_detail::finite(binding.transform.scale) || !semantic_detail::finite(binding.transform.center)
                    || !std::isfinite(binding.transform.rotation))
                    return false;
            }
            return true;
        }

        [[nodiscard]] static bool validateTextureRecord(const TextureRecord& record) noexcept
        {
            return record.revision.valid();
        }

        [[nodiscard]] static bool validateSkeletonRecord(const SkeletonRecord& record) noexcept
        {
            return record.revision.valid() && (!record.payload || validSkeletonPayload(*record.payload));
        }

        [[nodiscard]] static bool validateChunkRecord(const ChunkRecord& record) noexcept
        {
            return record.revision.valid();
        }

        [[nodiscard]] static bool validateLightRecord(const LightRecord& record) noexcept
        {
            return record.revision.valid() && std::isfinite(record.constantAttenuation)
                && std::isfinite(record.linearAttenuation) && std::isfinite(record.quadraticAttenuation)
                && std::isfinite(record.effectiveRadius) && record.effectiveRadius >= 0.0f
                && std::isfinite(record.actorFade);
        }

        [[nodiscard]] bool validateInstanceReferences(InstanceHandle self, const InstanceRecord& record) const noexcept
        {
            const bool hasMesh = record.mesh.valid();
            const bool hasModel = record.model.has_value();
            if (hasMesh == hasModel)
                return false;
            if (hasMesh && !mMeshes.contains(record.mesh))
                return false;
            if (hasModel && (!record.model->valid() || !mModels.contains(*record.model) || !record.materials.empty()))
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

            if (!record.attachment)
                return true;

            const AttachmentBinding& attachment = *record.attachment;
            if (!attachment.parent.valid() || attachment.parent == self || !mInstances.contains(attachment.parent))
                return false;
            if (attachment.skeleton && !mSkeletons.contains(*attachment.skeleton))
                return false;
            if (attachment.boneIndex && !attachment.skeleton)
                return false;
            if (attachment.boneIndex && attachment.skeleton)
            {
                const SkeletonRecord* skeleton = mSkeletons.get(*attachment.skeleton);
                if (skeleton && skeleton->payload && *attachment.boneIndex >= skeleton->payload->bones.size())
                    return false;
            }
            return !attachmentCycle(self, attachment.parent);
        }

        [[nodiscard]] bool attachmentCycle(InstanceHandle self, InstanceHandle parent) const noexcept
        {
            std::size_t remaining = mInstances.liveCount() + 1;
            InstanceHandle current = parent;
            while (current.valid() && remaining-- > 0)
            {
                if (current == self)
                    return true;
                const InstanceRecord* record = mInstances.get(current);
                if (!record || !record->attachment)
                    return false;
                current = record->attachment->parent;
            }
            return current.valid();
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
            mModels.forEachLive([&](ModelHandle, const ModelRecord& record) {
                if (!record.payload)
                    return;
                for (const ModelNodeRecord& node : record.payload->nodes)
                {
                    if (node.mesh && *node.mesh == handle)
                        referenced = true;
                }
            });
            return referenced;
        }

        [[nodiscard]] bool modelReferenced(ModelHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (record.model && *record.model == handle)
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
            mModels.forEachLive([&](ModelHandle, const ModelRecord& record) {
                if (!record.payload)
                    return;
                for (const ModelNodeRecord& node : record.payload->nodes)
                {
                    if (std::find(node.materials.begin(), node.materials.end(), handle) != node.materials.end())
                        referenced = true;
                }
            });
            return referenced;
        }

        [[nodiscard]] bool textureReferenced(TextureHandle handle) const noexcept
        {
            bool referenced = false;
            mMaterials.forEachLive([&](MaterialHandle, const MaterialRecord& record) {
                for (const TextureBinding& binding : record.textures)
                {
                    if (binding.texture == handle)
                        referenced = true;
                }
            });
            return referenced;
        }

        [[nodiscard]] bool skeletonReferenced(SkeletonHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if ((record.skeleton && *record.skeleton == handle)
                    || (record.attachment && record.attachment->skeleton && *record.attachment->skeleton == handle))
                    referenced = true;
            });
            return referenced;
        }

        [[nodiscard]] bool instanceReferenced(InstanceHandle handle) const noexcept
        {
            bool referenced = false;
            mInstances.forEachLive([&](InstanceHandle, const InstanceRecord& record) {
                if (record.attachment && record.attachment->parent == handle)
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
        ModelTable mModels;
        MaterialTable mMaterials;
        TextureTable mTextures;
        SkeletonTable mSkeletons;
        InstanceTable mInstances;
        ChunkTable mChunks;
        LightTable mLights;
    };

    static_assert(std::is_nothrow_move_assignable_v<RenderWorld>);
}

#endif
