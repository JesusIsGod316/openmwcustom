#ifndef OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITY_H
#define OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITY_H

#include "translationbundle.hpp"
#include "vfsidentity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace NifRender
{
    inline constexpr std::string_view WarningTextureSourceIdentity = "builtin:openmw-warning-image";
    inline constexpr std::string_view WarningTextureContentIdentity = "builtin:openmw-warning-image-v1";

    // Stage one already-resolved VFS image into the backend-neutral translation
    // bundle. Logical dedup is deliberately content-only: canonical source path
    // and archive ownership remain provenance, while color-space interpretation,
    // format class and sampler state live on material bindings and therefore do
    // not multiply TextureHandles. This is the ownership shape CP4 paging and
    // later CP7 persistent residency can reuse without path- or backend-key aliasing.
    [[nodiscard]] inline TextureIndex stageResolvedTexture(TranslationBundle& bundle, const ResolvedVfsIdentity& source,
        std::optional<std::uint32_t> sourceRecordId = std::nullopt)
    {
        if (!source.valid())
            return {};

        for (std::size_t i = 0; i < bundle.textures.size(); ++i)
        {
            if (bundle.textures[i].record.contentIdentity == source.contentIdentity)
                return TextureIndex{ static_cast<std::uint32_t>(i) };
        }

        TranslatedTexture texture;
        texture.record.sourceIdentity = std::string(source.canonicalPath.value());
        texture.record.contentIdentity = source.contentIdentity;
        texture.storage = TextureStorage::ExternalVfs;
        texture.sourceRecordId = sourceRecordId;
        bundle.textures.push_back(std::move(texture));
        return TextureIndex{ static_cast<std::uint32_t>(bundle.textures.size() - 1u) };
    }

    // ImageManager's missing/failed image contract is one process-global 8x8
    // magenta warning image. Give that compatibility asset a canonical content
    // identity so every missing authored path converges on one logical texture,
    // while diagnostics retain the actual failed source record/path separately.
    [[nodiscard]] inline TextureIndex stageWarningTexture(
        TranslationBundle& bundle, std::optional<std::uint32_t> sourceRecordId = std::nullopt)
    {
        for (std::size_t i = 0; i < bundle.textures.size(); ++i)
        {
            if (bundle.textures[i].record.contentIdentity == WarningTextureContentIdentity)
                return TextureIndex{ static_cast<std::uint32_t>(i) };
        }

        TranslatedTexture texture;
        texture.record.sourceIdentity = std::string(WarningTextureSourceIdentity);
        texture.record.contentIdentity = std::string(WarningTextureContentIdentity);
        texture.record.width = 8u;
        texture.record.height = 8u;
        texture.record.mipmapped = false;
        texture.storage = TextureStorage::WarningFallback;
        texture.sourceRecordId = sourceRecordId;
        bundle.textures.push_back(std::move(texture));
        return TextureIndex{ static_cast<std::uint32_t>(bundle.textures.size() - 1u) };
    }
}

#endif
