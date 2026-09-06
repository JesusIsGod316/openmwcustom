#include <components/nifrender/vfsidentity.hpp>
#include <components/rendercore/texturerealization.hpp>

#include <array>
#include <cassert>

int main()
{
    const std::array<std::uint64_t, 2> hash{ 0x1u, 0x2u };
    const std::string encoded = NifRender::encodeOpenMwContentHash(hash);
    assert(encoded == "openmw128:00000000000000010000000000000002");

    RenderCore::TextureBinding binding;
    binding.texture = RenderCore::TextureHandle::fromParts(12u, 3u);
    binding.colorSpace = RenderCore::TextureColorSpace::Srgb;
    binding.formatClass = RenderCore::TextureFormatClass::Color;

    RenderCore::TextureRecord texture;
    texture.sourceIdentity = "textures/a/shared.dds";
    texture.contentIdentity = "openmw128:aaa";
    texture.revision = RenderCore::ResourceRevision{ 7u };

    const RenderCore::TextureRealizationKey first = RenderCore::makeTextureRealizationKey(binding, texture);
    assert(first.valid());

    RenderCore::TextureRecord alias = texture;
    alias.sourceIdentity = "textures/b/alias.dds";
    alias.contentIdentity = "openmw128:bbb";
    const RenderCore::TextureRealizationKey aliasKey = RenderCore::makeTextureRealizationKey(binding, alias);
    assert(aliasKey == first);

    RenderCore::TextureRecord updated = texture;
    updated.revision = RenderCore::ResourceRevision{ 8u };
    const RenderCore::TextureRealizationKey updatedKey = RenderCore::makeTextureRealizationKey(binding, updated);
    assert(updatedKey != first);

    RenderCore::TextureBinding dataBinding = binding;
    dataBinding.colorSpace = RenderCore::TextureColorSpace::Data;
    const RenderCore::TextureRealizationKey dataKey = RenderCore::makeTextureRealizationKey(dataBinding, texture);
    assert(dataKey != first);

    RenderCore::TextureBinding replacementBinding = binding;
    replacementBinding.texture = RenderCore::TextureHandle::fromParts(13u, 1u);
    const RenderCore::TextureRealizationKey replacementKey
        = RenderCore::makeTextureRealizationKey(replacementBinding, texture);
    assert(replacementKey != first);

    return 0;
}
