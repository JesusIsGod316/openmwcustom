#ifndef OPENMW_COMPONENTS_NIFRENDER_ENCHANTEDGLOW_H
#define OPENMW_COMPONENTS_NIFRENDER_ENCHANTEDGLOW_H

#include "vfsidentity.hpp"

#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/updatebatch.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NifRender
{
    inline constexpr std::size_t EnchantedGlowFrameCount = 32u;

    enum class EnchantedGlowPublishStatus : std::uint8_t
    {
        Published,
        Reused,
        InvalidSource,
        MissingTexture,
        ExistingEnvironmentBinding,
        UnsupportedLightingOrder,
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
        [[nodiscard]] inline VFS::Path::Normalized framePath(std::size_t index)
        {
            std::ostringstream stream;
            stream << "textures/magicitem/caust" << std::setw(2) << std::setfill('0') << index << ".dds";
            return VFS::Path::Normalized(stream.str());
        }

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

        struct FrameResource
        {
            ResolvedVfsIdentity source;
            RenderCore::TextureHandle handle;
            bool reserved = false;
        };

        struct MaterialVariant
        {
            RenderCore::MaterialHandle source;
            RenderCore::MaterialHandle replacement;
            RenderCore::MaterialRecord record;
        };

        inline void cancelReservations(RenderCore::RenderWorld& world, RenderCore::ModelHandle model,
            const std::vector<MaterialVariant>& materials,
            const std::array<FrameResource, EnchantedGlowFrameCount>& frames) noexcept
        {
            if (model.valid())
                world.cancel(model);
            for (auto it = materials.rbegin(); it != materials.rend(); ++it)
                world.cancel(it->replacement);
            for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            {
                if (it->reserved)
                    world.cancel(it->handle);
            }
        }
    }

    // Publish one cached model/material variant that reproduces
    // SceneUtil::addEnchantedGlow without importing OSG state. All 32 legacy
    // caustic frames are neutral texture resources in exact source order. The
    // VSG compatibility backend chooses int(simulationTime * 16) % 32 without
    // rebuilding the static scene or consulting the VFS at render time.
    //
    // Current CP4F shader realization implements the established post-light
    // environment contribution. OpenMW's optional "apply lighting to environment
    // maps" mode moves the contribution before lighting; until that equation is
    // represented exactly, reject it here rather than publishing a semantically
    // indistinguishable variant that Vulkan would render incorrectly.
    [[nodiscard]] inline EnchantedGlowPublishResult publishEnchantedGlowVariant(RenderCore::RenderWorld& world,
        RenderCore::RenderWorldPublisher& publisher, const VFS::Manager& vfs, RenderCore::ModelHandle sourceModel,
        const RenderCore::Color& color, bool applyLightingToEnvironmentMaps = false)
    {
        using namespace RenderCore;
        if (!sourceModel.valid() || !semantic_detail::finite(color))
            return {};
        if (applyLightingToEnvironmentMaps)
            return { EnchantedGlowPublishStatus::UnsupportedLightingOrder, {} };
        const ModelRecord* source = world.get(sourceModel);
        if (!source || source->sourceIdentity.empty() || source->contentIdentity.empty() || !source->payload
            || !validModelPayloadStructure(*source->payload))
            return {};

        const std::string suffix = "#openmw-enchanted-glow-postlight:"
            + enchanted_glow_detail::colorIdentity(color);
        const std::string variantSourceIdentity = source->sourceIdentity + suffix;
        if (const std::optional<ModelHandle> existing
            = enchanted_glow_detail::findModelBySource(world, variantSourceIdentity))
            return { EnchantedGlowPublishStatus::Reused, *existing };

        std::array<enchanted_glow_detail::FrameResource, EnchantedGlowFrameCount> frames;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            frames[i].source = resolveTextureVfsIdentity(enchanted_glow_detail::framePath(i), vfs);
            if (!frames[i].source.valid())
                return { EnchantedGlowPublishStatus::MissingTexture, {} };
        }
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            if (const std::optional<TextureHandle> existing
                = enchanted_glow_detail::findTextureByContent(world, frames[i].source.contentIdentity))
            {
                frames[i].handle = *existing;
                continue;
            }
            // Two source frames can legally contain identical bytes. Reuse a
            // handle reserved earlier in this same unpublished batch so content
            // identity remains the canonical logical texture key.
            const auto duplicate = std::find_if(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(i),
                [&](const enchanted_glow_detail::FrameResource& value) {
                    return value.source.contentIdentity == frames[i].source.contentIdentity;
                });
            if (duplicate != frames.begin() + static_cast<std::ptrdiff_t>(i))
            {
                frames[i].handle = duplicate->handle;
                continue;
            }
            const std::optional<TextureHandle> reserved = world.reserveTexture();
            if (!reserved)
            {
                enchanted_glow_detail::cancelReservations(world, {}, {}, frames);
                return { EnchantedGlowPublishStatus::ReservationFailed, {} };
            }
            frames[i].handle = *reserved;
            frames[i].reserved = true;
        }

        std::vector<MaterialHandle> usedMaterials;
        for (const ModelNodeRecord& node : source->payload->nodes)
        {
            for (const MaterialHandle material : node.materials)
            {
                if (std::find(usedMaterials.begin(), usedMaterials.end(), material) == usedMaterials.end())
                    usedMaterials.push_back(material);
            }
        }

        std::vector<enchanted_glow_detail::MaterialVariant> materials;
        materials.reserve(usedMaterials.size());
        for (const MaterialHandle materialHandle : usedMaterials)
        {
            const MaterialRecord* baseMaterial = world.get(materialHandle);
            if (!baseMaterial)
            {
                enchanted_glow_detail::cancelReservations(world, {}, materials, frames);
                return {};
            }
            if (std::any_of(baseMaterial->textures.begin(), baseMaterial->textures.end(),
                    [](const TextureBinding& binding) { return binding.role == TextureRole::Environment; }))
            {
                enchanted_glow_detail::cancelReservations(world, {}, materials, frames);
                return { EnchantedGlowPublishStatus::ExistingEnvironmentBinding, {} };
            }
            const std::optional<MaterialHandle> replacement = world.reserveMaterial();
            if (!replacement)
            {
                enchanted_glow_detail::cancelReservations(world, {}, materials, frames);
                return { EnchantedGlowPublishStatus::ReservationFailed, {} };
            }

            MaterialRecord record = *baseMaterial;
            record.revision = InitialResourceRevision;
            record.sourceIdentity = baseMaterial->sourceIdentity + suffix;
            record.environmentMapColor = color;
            record.environmentMapStrength = 1.0f;
            record.textures.reserve(record.textures.size() + frames.size());
            for (const auto& frame : frames)
            {
                TextureBinding binding;
                binding.role = TextureRole::Environment;
                binding.texture = frame.handle;
                binding.colorSpace = TextureColorSpace::Srgb;
                binding.formatClass = TextureFormatClass::Color;
                binding.sampler.wrapU = TextureWrap::Repeat;
                binding.sampler.wrapV = TextureWrap::Repeat;
                record.textures.push_back(std::move(binding));
            }
            materials.push_back({ materialHandle, *replacement, std::move(record) });
        }

        const std::optional<ModelHandle> variantModel = world.reserveModel();
        if (!variantModel)
        {
            enchanted_glow_detail::cancelReservations(world, {}, materials, frames);
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
                    enchanted_glow_detail::cancelReservations(world, *variantModel, materials, frames);
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
        for (const auto& frame : frames)
        {
            if (!frame.reserved)
                continue;
            TextureRecord texture;
            texture.sourceIdentity = std::string(frame.source.canonicalPath.value());
            texture.contentIdentity = frame.source.contentIdentity;
            built = built && batch.add(CreateTexture{ frame.handle, std::move(texture) });
        }
        for (auto& material : materials)
            built = built && batch.add(CreateMaterial{ material.replacement, std::move(material.record) });
        built = built && batch.add(CreateModel{ *variantModel, std::move(model) });
        if (!built || !batch.seal())
        {
            enchanted_glow_detail::cancelReservations(world, *variantModel, materials, frames);
            return { EnchantedGlowPublishStatus::BatchBuildFailed, {} };
        }
        if (publisher.apply(batch) != PublishStatus::Applied)
        {
            enchanted_glow_detail::cancelReservations(world, *variantModel, materials, frames);
            return { EnchantedGlowPublishStatus::PublishRejected, {} };
        }
        return { EnchantedGlowPublishStatus::Published, *variantModel };
    }
}

#endif
