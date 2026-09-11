#ifndef OPENMW_COMPONENTS_NIFRENDER_ENCHANTEDGLOW_H
#define OPENMW_COMPONENTS_NIFRENDER_ENCHANTEDGLOW_H

#include "vfsidentity.hpp"

#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/updatebatch.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace NifRender
{
    inline constexpr VFS::Path::NormalizedView EnchantedGlowFirstFrame("textures/magicitem/caust00.dds");

    enum class EnchantedGlowPublishStatus : std::uint8_t
    {
        Published,
        Reused,
        InvalidSource,
        MissingTexture,
        ExistingEnvironmentBinding,
        ReservationFailed,
        BatchBuildFailed,
        PublishRejected,
    };

    struct EnchantedGlowPublishResult
    {
        EnchantedGlowPublishStatus status = EnchantedGlowPublishStatus::InvalidSource;
        RenderCore::ModelHandle model;

        [[nodiscard]] bool available() const noexcept
        {
            return status == EnchantedGlowPublishStatus::Published || status == EnchantedGlowPublishStatus::Reused;
        }
    };

    namespace enchanted_glow_detail
    {
        [[nodiscard]] inline std::string colorIdentity(const RenderCore::Color& color)
        {
            std::ostringstream stream;
            stream << std::hex << std::bit_cast<std::uint32_t>(color.r) << '-'
                   << std::bit_cast<std::uint32_t>(color.g) << '-'
                   << std::bit_cast<std::uint32_t>(color.b) << '-'
                   << std::bit_cast<std::uint32_t>(color.a);
            return stream.str();
        }

        [[nodiscard]] inline std::optional<RenderCore::TextureHandle> findTextureByContent(
            const RenderCore::RenderWorld& world, std::string_view contentIdentity) noexcept
        {
            std::optional<RenderCore::TextureHandle> result;
            world.forEachTexture([&](RenderCore::TextureHandle handle, const RenderCore::TextureRecord& record) {
                if (!result && record.contentIdentity == contentIdentity)
                    result = handle;
            });
            return result;
        }

        [[nodiscard]] inline std::optional<RenderCore::ModelHandle> findModelBySource(
            const RenderCore::RenderWorld& world, std::string_view sourceIdentity) noexcept
        {
            std::optional<RenderCore::ModelHandle> result;
            world.forEachModel([&](RenderCore::ModelHandle handle, const RenderCore::ModelRecord& record) {
                if (!result && record.sourceIdentity == sourceIdentity)
                    result = handle;
            });
            return result;
        }

        struct MaterialVariant
        {
            RenderCore::MaterialHandle source;
            RenderCore::MaterialHandle replacement;
            RenderCore::MaterialRecord record;
        };
    }

    // Publish one cached model/material variant that reproduces
    // SceneUtil::addEnchantedGlow without importing OSG state. The first caustic
    // frame is the neutral texture identity; the VSG compatibility backend owns
    // the legacy 32-frame, 16 Hz runtime selection for this exact source family.
    [[nodiscard]] inline EnchantedGlowPublishResult publishEnchantedGlowVariant(RenderCore::RenderWorld& world,
        RenderCore::RenderWorldPublisher& publisher, const VFS::Manager& vfs, RenderCore::ModelHandle sourceModel,
        const RenderCore::Color& color)
    {
        using namespace RenderCore;
        if (!sourceModel.valid() || !semantic_detail::finite(color))
            return {};
        const ModelRecord* source = world.get(sourceModel);
        if (!source || !source->payload || !validModelPayloadStructure(*source->payload))
            return {};

        const ResolvedVfsIdentity caustic = resolveTextureVfsIdentity(EnchantedGlowFirstFrame, vfs);
        if (!caustic.valid())
            return { EnchantedGlowPublishStatus::MissingTexture, {} };

        const std::string suffix = "#openmw-enchanted-glow:" + enchanted_glow_detail::colorIdentity(color);
        const std::string variantSourceIdentity = source->sourceIdentity + suffix;
        if (const std::optional<ModelHandle> existing
            = enchanted_glow_detail::findModelBySource(world, variantSourceIdentity))
            return { EnchantedGlowPublishStatus::Reused, *existing };

        std::vector<MaterialHandle> usedMaterials;
        for (const ModelNodeRecord& node : source->payload->nodes)
        {
            for (const MaterialHandle material : node.materials)
            {
                if (std::find(usedMaterials.begin(), usedMaterials.end(), material) == usedMaterials.end())
                    usedMaterials.push_back(material);
            }
        }

        std::optional<TextureHandle> causticTexture
            = enchanted_glow_detail::findTextureByContent(world, caustic.contentIdentity);
        bool reservedTexture = false;
        if (!causticTexture)
        {
            causticTexture = world.reserveTexture();
            if (!causticTexture)
                return { EnchantedGlowPublishStatus::ReservationFailed, {} };
            reservedTexture = true;
        }

        std::vector<enchanted_glow_detail::MaterialVariant> materials;
        materials.reserve(usedMaterials.size());
        for (const MaterialHandle materialHandle : usedMaterials)
        {
            const MaterialRecord* baseMaterial = world.get(materialHandle);
            if (!baseMaterial)
            {
                if (reservedTexture)
                    world.cancel(*causticTexture);
                for (const auto& material : materials)
                    world.cancel(material.replacement);
                return {};
            }
            if (std::ranges::any_of(baseMaterial->textures,
                    [](const TextureBinding& binding) { return binding.role == TextureRole::Environment; }))
            {
                if (reservedTexture)
                    world.cancel(*causticTexture);
                for (const auto& material : materials)
                    world.cancel(material.replacement);
                return { EnchantedGlowPublishStatus::ExistingEnvironmentBinding, {} };
            }
            const std::optional<MaterialHandle> replacement = world.reserveMaterial();
            if (!replacement)
            {
                if (reservedTexture)
                    world.cancel(*causticTexture);
                for (const auto& material : materials)
                    world.cancel(material.replacement);
                return { EnchantedGlowPublishStatus::ReservationFailed, {} };
            }

            MaterialRecord record = *baseMaterial;
            record.revision = InitialResourceRevision;
            record.sourceIdentity = baseMaterial->sourceIdentity + suffix;
            record.environmentMapColor = color;
            record.environmentMapStrength = 1.0f;
            TextureBinding binding;
            binding.role = TextureRole::Environment;
            binding.texture = *causticTexture;
            binding.colorSpace = TextureColorSpace::Srgb;
            binding.formatClass = TextureFormatClass::Color;
            binding.sampler.wrapU = TextureWrap::Repeat;
            binding.sampler.wrapV = TextureWrap::Repeat;
            record.textures.push_back(std::move(binding));
            materials.push_back({ materialHandle, *replacement, std::move(record) });
        }

        const std::optional<ModelHandle> variantModel = world.reserveModel();
        if (!variantModel)
        {
            if (reservedTexture)
                world.cancel(*causticTexture);
            for (const auto& material : materials)
                world.cancel(material.replacement);
            return { EnchantedGlowPublishStatus::ReservationFailed, {} };
        }

        auto payload = std::make_shared<ModelPayload>(*source->payload);
        for (ModelNodeRecord& node : payload->nodes)
        {
            for (MaterialHandle& material : node.materials)
            {
                const auto replacement = std::find_if(materials.begin(), materials.end(),
                    [&](const enchanted_glow_detail::MaterialVariant& value) { return value.source == material; });
                if (replacement == materials.end())
                {
                    world.cancel(*variantModel);
                    if (reservedTexture)
                        world.cancel(*causticTexture);
                    for (const auto& value : materials)
                        world.cancel(value.replacement);
                    return {};
                }
                material = replacement->replacement;
            }
        }

        ModelRecord model = *source;
        model.revision = InitialResourceRevision;
        model.sourceIdentity = variantSourceIdentity;
        model.contentIdentity = source->contentIdentity + suffix;
        model.payload = std::move(payload);

        RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence(), variantSourceIdentity);
        bool built = true;
        if (reservedTexture)
        {
            TextureRecord texture;
            texture.sourceIdentity = std::string(caustic.canonicalPath.value());
            texture.contentIdentity = caustic.contentIdentity;
            built = batch.add(CreateTexture{ *causticTexture, std::move(texture) });
        }
        for (auto& material : materials)
            built = built && batch.add(CreateMaterial{ material.replacement, std::move(material.record) });
        built = built && batch.add(CreateModel{ *variantModel, std::move(model) });
        if (!built || !batch.seal())
        {
            world.cancel(*variantModel);
            if (reservedTexture)
                world.cancel(*causticTexture);
            for (const auto& material : materials)
                world.cancel(material.replacement);
            return { EnchantedGlowPublishStatus::BatchBuildFailed, {} };
        }
        if (publisher.apply(batch) != PublishStatus::Applied)
        {
            world.cancel(*variantModel);
            if (reservedTexture)
                world.cancel(*causticTexture);
            for (const auto& material : materials)
                world.cancel(material.replacement);
            return { EnchantedGlowPublishStatus::PublishRejected, {} };
        }
        return { EnchantedGlowPublishStatus::Published, *variantModel };
    }
}

#endif
