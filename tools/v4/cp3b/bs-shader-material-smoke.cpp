#include <components/nifrender/shadermaterialpass.hpp>

#include <cassert>
#include <cmath>

namespace
{
    [[nodiscard]] bool near(float lhs, float rhs) noexcept
    {
        return std::fabs(lhs - rhs) < 0.00001f;
    }
}

int main()
{
    const auto base = NifRender::bsTextureSetStageSemantic(0u);
    const auto normal = NifRender::bsTextureSetStageSemantic(1u);
    const auto glow = NifRender::bsTextureSetStageSemantic(2u);
    assert(base && base->role == RenderCore::TextureRole::Diffuse
        && base->colorSpace == RenderCore::TextureColorSpace::Srgb);
    assert(normal && normal->role == RenderCore::TextureRole::Normal
        && normal->colorSpace == RenderCore::TextureColorSpace::Data
        && normal->formatClass == RenderCore::TextureFormatClass::Normal);
    assert(glow && glow->role == RenderCore::TextureRole::Emissive);
    assert(!NifRender::bsTextureSetStageSemantic(3u));
    assert(!NifRender::bsTextureSetStageSemantic(7u));

    const RenderCore::TextureTransform effectTransform
        = NifRender::makeBsEffectTextureTransform({ 0.25f, -0.5f }, { 2.0f, 3.0f });
    assert(near(effectTransform.offset.x, -0.25f));
    assert(near(effectTransform.offset.y, 0.5f));
    assert(near(effectTransform.scale.x, 2.0f));
    assert(near(effectTransform.scale.y, 3.0f));
    assert(near(effectTransform.center.x, 0.5f) && near(effectTransform.center.y, 0.5f));
    assert(effectTransform.uvSet == 0u);
    assert(effectTransform.convention == RenderCore::TextureTransformConvention::Direct);

    NifRender::DrawableMaterialTranslation translated;
    NifRender::initializeDrawableMaterial(translated, false);
    translated.material.state.specular = { 1.0f, 1.0f, 1.0f, 1.0f };
    translated.material.state.shininess = 80.0f;
    translated.material.supplement.refraction = true;
    translated.material.supplement.refractionStrength = 9.0f;

    NifRender::ShaderMaterialSemantic lighting;
    lighting.kind = NifRender::ShaderMaterialKind::Lighting;
    lighting.transparency = 0.75f;
    lighting.alphaBlend = true;
    lighting.sourceBlendMode = 6u;
    lighting.destinationBlendMode = 7u;
    lighting.alphaTest = true;
    lighting.alphaTestThreshold = 128u;
    lighting.decal = true;
    lighting.depthTest = false;
    lighting.depthWrite = true;
    lighting.emission = { 0.1f, 0.2f, 0.3f, 1.0f };
    lighting.emissiveMultiplier = 2.5f;
    lighting.treeAnimation = true;

    assert(NifRender::applyShaderMaterialSemantic(lighting, translated));
    assert(near(translated.material.state.alpha, 0.75f));
    assert(translated.material.state.alphaBlendEnabled);
    assert(translated.material.state.alphaTestEnabled);
    assert(translated.material.state.alphaCompare == RenderCore::CompareOp::Greater);
    assert(translated.material.state.alphaMode == RenderCore::AlphaMode::Blend);
    assert(translated.material.state.sourceBlend == RenderCore::BlendFactor::SourceAlpha);
    assert(translated.material.state.destinationBlend == RenderCore::BlendFactor::OneMinusSourceAlpha);
    assert(translated.material.state.transparentSort == RenderCore::TransparentSortPolicy::Sorted);
    assert(near(translated.material.state.alphaCutoff, 128.0f / 255.0f));
    assert(near(translated.material.state.emission.x, 0.1f));
    assert(near(translated.material.state.emissiveMultiplier, 2.5f));
    assert(near(translated.material.state.specular.x, 0.0f));
    assert(near(translated.material.state.shininess, 0.0f));
    assert(translated.material.supplement.decal);
    assert(translated.material.supplement.treeAnimation);
    assert(!translated.material.supplement.refraction);

    // Geometry-local NiAlphaProperty is ordered after the BS shader property in
    // V3.25, so it must still be able to replace external material alpha state.
    const std::uint16_t alphaFlags = static_cast<std::uint16_t>(Nif::NiAlphaProperty::Flag_Testing)
        | static_cast<std::uint16_t>(1u << 10u);
    NifRender::applyDrawableAlphaSemantics(alphaFlags, 64u, translated);
    assert(!translated.material.state.alphaBlendEnabled);
    assert(translated.material.state.alphaTestEnabled);
    assert(translated.material.state.alphaCompare == RenderCore::CompareOp::Less);
    assert(translated.material.state.alphaMode == RenderCore::AlphaMode::Mask);
    assert(near(translated.material.state.alphaCutoff, 64.0f / 255.0f));

    NifRender::DrawableMaterialTranslation effectTarget;
    NifRender::initializeDrawableMaterial(effectTarget, false);
    NifRender::ShaderMaterialSemantic effect;
    effect.kind = NifRender::ShaderMaterialKind::Effect;
    effect.falloff = true;
    effect.falloffParams = { 1.0f, 2.0f, 3.0f, 4.0f };
    effect.softEffect = true;
    effect.softEffectDepth = 7.0f;
    effect.emission = { 0.4f, 0.5f, 0.6f, 1.0f };
    assert(NifRender::applyShaderMaterialSemantic(effect, effectTarget));
    assert(effectTarget.material.supplement.falloff);
    assert(near(effectTarget.material.supplement.falloffParams.w, 4.0f));
    assert(effectTarget.material.supplement.softEffect);
    assert(near(effectTarget.material.supplement.softEffectDepth, 7.0f));
    assert(near(effectTarget.material.state.emission.z, 0.6f));

    NifRender::DrawableMaterialTranslation invalid;
    NifRender::initializeDrawableMaterial(invalid, false);
    NifRender::ShaderMaterialSemantic badBlend;
    badBlend.alphaBlend = true;
    badBlend.sourceBlendMode = 99u;
    assert(!NifRender::applyShaderMaterialSemantic(badBlend, invalid));
    assert((invalid.issues
               & NifRender::drawableMaterialIssue(NifRender::DrawableMaterialIssue::InvalidPackedAlpha))
        != 0u);

    return 0;
}
