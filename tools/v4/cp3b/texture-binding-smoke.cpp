#include <components/nifrender/texturepass.hpp>
#include <components/rendercore/renderworld.hpp>

#include <cassert>

int main()
{
    using namespace NifRender;

    const auto base = legacyTextureStageSemantic(Nif::NiTexturingProperty::BaseTexture);
    assert(base);
    assert(base->role == RenderCore::TextureRole::Diffuse);
    assert(base->colorSpace == RenderCore::TextureColorSpace::Srgb);
    assert(base->formatClass == RenderCore::TextureFormatClass::Color);

    const auto gloss = legacyTextureStageSemantic(Nif::NiTexturingProperty::GlossTexture);
    assert(gloss);
    assert(gloss->role == RenderCore::TextureRole::Gloss);
    assert(gloss->colorSpace == RenderCore::TextureColorSpace::Data);
    assert(gloss->formatClass == RenderCore::TextureFormatClass::Color);

    const auto bump = legacyTextureStageSemantic(Nif::NiTexturingProperty::BumpTexture);
    assert(bump);
    assert(bump->role == RenderCore::TextureRole::Bump);
    assert(bump->colorSpace == RenderCore::TextureColorSpace::Data);
    assert(bump->formatClass == RenderCore::TextureFormatClass::Unknown);
    assert(!legacyTextureStageSemantic(999u));

    const RenderCore::SamplerSemantic sampler = legacyTextureSampler(true, false);
    assert(sampler.wrapU == RenderCore::TextureWrap::Repeat);
    assert(sampler.wrapV == RenderCore::TextureWrap::Clamp);
    assert(sampler.minFilter == RenderCore::TextureFilter::Linear);
    assert(sampler.magFilter == RenderCore::TextureFilter::Linear);
    assert(sampler.mipmapMode == RenderCore::TextureMipmapMode::Linear);
    assert(sampler.maxAnisotropy == 0.0f);

    const auto binding = makeLegacyTextureBinding(
        TextureIndex{ 3u }, Nif::NiTexturingProperty::BaseTexture, true, false, 2u);
    assert(binding);
    assert(binding->texture == TextureIndex{ 3u });
    assert(binding->role == RenderCore::TextureRole::Diffuse);
    assert(binding->transform.uvSet == 2u);
    assert(binding->transform.convention == RenderCore::TextureTransformConvention::Direct);
    assert(binding->sampler.wrapU == RenderCore::TextureWrap::Repeat);
    assert(binding->sampler.wrapV == RenderCore::TextureWrap::Clamp);

    TranslationBundle bundle;
    const TextureIndex firstWarning = stageWarningTexture(bundle, 7u);
    const TextureIndex secondWarning = stageWarningTexture(bundle, 8u);
    assert(firstWarning.valid());
    assert(firstWarning == secondWarning);
    assert(bundle.textures.size() == 1u);
    assert(bundle.textures.front().storage == TextureStorage::WarningFallback);
    assert(bundle.textures.front().record.sourceIdentity == WarningTextureSourceIdentity);
    assert(bundle.textures.front().record.contentIdentity == WarningTextureContentIdentity);
    assert(bundle.textures.front().record.width == 8u);
    assert(bundle.textures.front().record.height == 8u);
    assert(!bundle.textures.front().record.mipmapped);
    assert(bundle.valid());

    RenderCore::RenderWorld world;
    const auto unresolved = world.reserveTexture();
    assert(unresolved);
    assert(!world.commit(*unresolved,
        RenderCore::TextureRecord{ .sourceIdentity = "textures/source-only.dds" }));
    assert(world.cancel(*unresolved));

    const auto warning = world.reserveTexture();
    assert(warning);
    assert(world.commit(*warning, bundle.textures.front().record));
    assert(world.valid());

    return 0;
}
