#include <components/nifrender/textureidentity.hpp>

#include <cassert>

int main()
{
    using namespace NifRender;

    TranslationBundle bundle;

    ResolvedVfsIdentity first;
    first.canonicalPath = VFS::Path::Normalized("textures/a/shared.dds");
    first.archiveIdentity = "loose";
    first.contentIdentity = "openmw128:11111111111111112222222222222222";

    ResolvedVfsIdentity alias;
    alias.canonicalPath = VFS::Path::Normalized("textures/b/alias.dds");
    alias.archiveIdentity = "archive.bsa";
    alias.contentIdentity = first.contentIdentity;

    const TextureIndex firstIndex = stageResolvedTexture(bundle, first, 10u);
    const TextureIndex aliasIndex = stageResolvedTexture(bundle, alias, 20u);
    assert(firstIndex.valid());
    assert(aliasIndex == firstIndex);
    assert(bundle.textures.size() == 1u);
    assert(bundle.textures.front().record.sourceIdentity == "textures/a/shared.dds");
    assert(bundle.textures.front().record.contentIdentity == first.contentIdentity);
    assert(bundle.textures.front().sourceRecordId == 10u);

    ResolvedVfsIdentity second;
    second.canonicalPath = VFS::Path::Normalized("textures/c/other.dds");
    second.archiveIdentity = "archive.bsa";
    second.contentIdentity = "openmw128:33333333333333334444444444444444";
    const TextureIndex secondIndex = stageResolvedTexture(bundle, second, 30u);
    assert(secondIndex.valid());
    assert(secondIndex != firstIndex);
    assert(bundle.textures.size() == 2u);

    ResolvedVfsIdentity unresolved;
    unresolved.canonicalPath = VFS::Path::Normalized("textures/missing.dds");
    const TextureIndex invalid = stageResolvedTexture(bundle, unresolved, 40u);
    assert(!invalid.valid());
    assert(bundle.textures.size() == 2u);

    TranslatedMaterial material;
    material.state.sourceIdentity = "material:shared-interpretations";

    TranslatedTextureBinding color;
    color.texture = firstIndex;
    color.role = RenderCore::TextureRole::Diffuse;
    color.colorSpace = RenderCore::TextureColorSpace::Srgb;
    color.formatClass = RenderCore::TextureFormatClass::Color;
    color.sampler.wrapU = RenderCore::TextureWrap::Repeat;
    color.sampler.wrapV = RenderCore::TextureWrap::Repeat;
    material.textures.push_back(color);

    TranslatedTextureBinding data;
    data.texture = firstIndex;
    data.role = RenderCore::TextureRole::Normal;
    data.colorSpace = RenderCore::TextureColorSpace::Data;
    data.formatClass = RenderCore::TextureFormatClass::Normal;
    data.sampler.wrapU = RenderCore::TextureWrap::Clamp;
    data.sampler.wrapV = RenderCore::TextureWrap::Clamp;
    data.sampler.maxAnisotropy = 8.0f;
    material.textures.push_back(data);

    bundle.materials.push_back(material);
    assert(bundle.valid());
    assert(bundle.materials.front().textures[0].texture == bundle.materials.front().textures[1].texture);
    assert(bundle.materials.front().textures[0].colorSpace != bundle.materials.front().textures[1].colorSpace);
    assert(bundle.materials.front().textures[0].sampler.wrapU != bundle.materials.front().textures[1].sampler.wrapU);
    assert(bundle.textures.size() == 2u);

    return 0;
}
