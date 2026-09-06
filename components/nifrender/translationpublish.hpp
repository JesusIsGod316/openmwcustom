#ifndef OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONPUBLISH_H
#define OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONPUBLISH_H

#include "translationbundle.hpp"

#include <components/rendercore/updatebatch.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace NifRender
{
    enum class TranslationPublishStatus : std::uint8_t
    {
        Applied,
        InvalidBundle,
        TranslationErrors,
        ReservationFailed,
        BatchBuildFailed,
        PublishRejected,
    };

    struct TranslationBinding
    {
        std::vector<RenderCore::TextureHandle> textures;
        std::vector<RenderCore::MaterialHandle> materials;
        std::vector<RenderCore::MeshHandle> meshes;
        RenderCore::ModelHandle model;
    };

    struct TranslationPublishResult
    {
        TranslationPublishStatus status = TranslationPublishStatus::InvalidBundle;
        RenderCore::PublishStatus worldStatus = RenderCore::PublishStatus::OperationRejected;
        TranslationBinding binding;

        [[nodiscard]] bool applied() const noexcept { return status == TranslationPublishStatus::Applied; }
    };

    namespace publish_detail
    {
        inline void cancelReservations(RenderCore::RenderWorld& world, const TranslationBinding& binding) noexcept
        {
            if (binding.model.valid())
                world.cancel(binding.model);
            for (auto it = binding.meshes.rbegin(); it != binding.meshes.rend(); ++it)
                world.cancel(*it);
            for (auto it = binding.materials.rbegin(); it != binding.materials.rend(); ++it)
                world.cancel(*it);
            for (auto it = binding.textures.rbegin(); it != binding.textures.rend(); ++it)
                world.cancel(*it);
        }

        template <class Handle, class Reserve>
        [[nodiscard]] bool reserveMany(std::size_t count, std::vector<Handle>& destination, Reserve&& reserve)
        {
            destination.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::optional<Handle> handle = reserve();
                if (!handle)
                    return false;
                destination.push_back(*handle);
            }
            return true;
        }
    }

    // Main-thread deterministic publication seam. The source translation can be
    // built/cached on workers; only this short binding step touches RenderWorld.
    // Local resource order is preserved exactly so repeated translation of the
    // same bundle yields deterministic operation order independent of worker
    // completion timing.
    [[nodiscard]] inline TranslationPublishResult publishTranslation(RenderCore::RenderWorld& world,
        RenderCore::RenderWorldPublisher& publisher, const TranslationBundle& bundle, RenderCore::UpdateSequence sequence)
    {
        TranslationPublishResult result;
        if (!bundle.valid())
        {
            result.status = TranslationPublishStatus::InvalidBundle;
            return result;
        }
        if (bundle.hasErrors())
        {
            result.status = TranslationPublishStatus::TranslationErrors;
            return result;
        }

        TranslationBinding binding;
        if (!publish_detail::reserveMany(bundle.textures.size(), binding.textures, [&] { return world.reserveTexture(); })
            || !publish_detail::reserveMany(
                bundle.materials.size(), binding.materials, [&] { return world.reserveMaterial(); })
            || !publish_detail::reserveMany(bundle.meshes.size(), binding.meshes, [&] { return world.reserveMesh(); }))
        {
            publish_detail::cancelReservations(world, binding);
            result.status = TranslationPublishStatus::ReservationFailed;
            return result;
        }

        const std::optional<RenderCore::ModelHandle> model = world.reserveModel();
        if (!model)
        {
            publish_detail::cancelReservations(world, binding);
            result.status = TranslationPublishStatus::ReservationFailed;
            return result;
        }
        binding.model = *model;

        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), sequence, bundle.sourceIdentity);
        bool built = true;
        for (std::size_t i = 0; i < bundle.textures.size() && built; ++i)
            built = batch.add(RenderCore::CreateTexture{ binding.textures[i], bundle.textures[i].record });

        for (std::size_t i = 0; i < bundle.materials.size() && built; ++i)
        {
            RenderCore::MaterialRecord record = bundle.materials[i].state;
            record.textures.reserve(bundle.materials[i].textures.size());
            for (const TranslatedTextureBinding& source : bundle.materials[i].textures)
            {
                RenderCore::TextureBinding translated;
                translated.role = source.role;
                translated.texture = binding.textures[source.texture.value()];
                translated.colorSpace = source.colorSpace;
                translated.formatClass = source.formatClass;
                translated.transform = source.transform;
                translated.sampler = source.sampler;
                record.textures.push_back(std::move(translated));
            }
            built = batch.add(RenderCore::CreateMaterial{ binding.materials[i], std::move(record) });
        }

        for (std::size_t i = 0; i < bundle.meshes.size() && built; ++i)
            built = batch.add(RenderCore::CreateMesh{ binding.meshes[i], bundle.meshes[i].record });

        if (built)
        {
            auto payload = std::make_shared<RenderCore::ModelPayload>();
            payload->roots = bundle.model.roots;
            payload->nodes.reserve(bundle.model.nodes.size());
            for (const TranslatedModelNode& source : bundle.model.nodes)
            {
                RenderCore::ModelNodeRecord translated;
                translated.name = source.name;
                translated.sourceRecordId = source.sourceRecordId;
                translated.parent = source.parent;
                translated.localTransform = source.localTransform;
                translated.kind = source.kind;
                if (source.mesh)
                    translated.mesh = binding.meshes[source.mesh->value()];
                translated.materials.reserve(source.materials.size());
                for (const MaterialIndex material : source.materials)
                    translated.materials.push_back(binding.materials[material.value()]);
                translated.activeSwitchChild = source.activeSwitchChild;
                translated.lod = source.lod;
                translated.billboard = source.billboard;
                translated.sort = source.sort;
                translated.flags = source.flags;
                payload->nodes.push_back(std::move(translated));
            }

            RenderCore::ModelRecord record;
            record.sourceIdentity = bundle.model.sourceIdentity;
            record.contentIdentity = bundle.model.contentIdentity;
            record.bounds = bundle.model.bounds;
            record.payload = std::move(payload);
            built = batch.add(RenderCore::CreateModel{ binding.model, std::move(record) });
        }

        if (!built || !batch.seal())
        {
            publish_detail::cancelReservations(world, binding);
            result.status = TranslationPublishStatus::BatchBuildFailed;
            return result;
        }

        result.worldStatus = publisher.apply(batch);
        if (result.worldStatus != RenderCore::PublishStatus::Applied)
        {
            publish_detail::cancelReservations(world, binding);
            result.status = TranslationPublishStatus::PublishRejected;
            return result;
        }

        result.status = TranslationPublishStatus::Applied;
        result.binding = std::move(binding);
        return result;
    }
}

#endif
