#ifndef OPENMW_COMPONENTS_RENDERCORE_TEXTUREREALIZATION_H
#define OPENMW_COMPONENTS_RENDERCORE_TEXTUREREALIZATION_H

#include "realizationkeys.hpp"

#include <cstddef>
#include <cstdint>

namespace RenderCore
{
    // Backend texture residency/view identity. A logical TextureHandle survives
    // updates, so the resource revision is required alongside the interpretation
    // variant to prevent stale backend images or views from being reused.
    struct TextureRealizationKey
    {
        TextureViewKey view;
        ResourceRevision revision = InitialResourceRevision;

        [[nodiscard]] bool valid() const noexcept { return view.valid() && revision.valid(); }
        friend bool operator==(const TextureRealizationKey&, const TextureRealizationKey&) = default;
    };

    [[nodiscard]] inline TextureRealizationKey makeTextureRealizationKey(
        const TextureBinding& binding, const TextureRecord& texture) noexcept
    {
        return { makeTextureViewKey(binding), texture.revision };
    }

    [[nodiscard]] inline std::uint64_t stableTextureRealizationFingerprint(const TextureRealizationKey& key) noexcept
    {
        std::uint64_t hash = realization_key_detail::FnvOffset;
        realization_key_detail::observeInteger(hash, RealizationKeySchemaRevision);
        realization_key_detail::observeInteger(hash, key.view.texture.slot());
        realization_key_detail::observeInteger(hash, key.view.texture.generation());
        realization_key_detail::observeInteger(hash, key.revision.value());
        realization_key_detail::observeEnum(hash, key.view.colorSpace);
        realization_key_detail::observeEnum(hash, key.view.formatClass);
        return hash;
    }

    struct TextureRealizationKeyHash
    {
        [[nodiscard]] std::size_t operator()(const TextureRealizationKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableTextureRealizationFingerprint(key));
        }
    };
}

#endif
