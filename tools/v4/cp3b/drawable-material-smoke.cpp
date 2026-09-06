#include <components/nifrender/drawablematerial.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    NifRender::DrawableMaterialTranslation buildLegacyMaterial(bool hasVertexColors, unsigned int nifVersion)
    {
        NifRender::DrawableMaterialTranslation translated;
        NifRender::initializeDrawableMaterial(translated, hasVertexColors);

        bool specularEnabled = true;
        auto lightMode = Nif::NiVertexColorProperty::LightMode::LightMode_EmiAmbDif;

        NifRender::applyLegacyMaterialSemantics(translated, { 0.2f, 0.3f, 0.4f }, { 0.1f, 0.2f, 0.3f },
            { 0.8f, 0.7f, 0.6f }, { 0.05f, 0.04f, 0.03f }, 42.0f, 0.75f, 1.0f, false);
        NifRender::applyLegacyMaterialSemantics(translated, { 0.6f, 0.5f, 0.4f }, { 0.3f, 0.2f, 0.1f },
            { 0.9f, 0.8f, 0.7f }, { 0.2f, 0.1f, 0.0f }, 64.0f, 0.5f, 1.0f, false);

        NifRender::applyVertexColorSemantics(Nif::NiVertexColorProperty::VertexMode::VertMode_SrcAmbDif,
            Nif::NiVertexColorProperty::LightMode::LightMode_EmiAmbDif, translated.material.state, lightMode);
        NifRender::finalizeDrawableMaterial(translated, hasVertexColors, nifVersion, specularEnabled, lightMode);
        return translated;
    }
}

int main()
{
    auto translated = buildLegacyMaterial(true, Nif::NIFFile::VER_OB);
    assert(translated.publishableStaticState());
    assert(translated.material.state.diffuse.r == 0.6f);
    assert(translated.material.state.diffuse.a == 0.5f);
    assert(translated.material.state.shininess == 64.0f);
    assert(translated.material.state.vertexColorMode == RenderCore::VertexColorMode::AmbientDiffuse);

    auto mw = buildLegacyMaterial(true, Nif::NIFFile::VER_MW);
    assert(mw.material.state.specular.r == 0.0f);
    assert(mw.material.state.shininess == 0.0f);

    auto withoutColors = buildLegacyMaterial(false, Nif::NIFFile::VER_OB);
    assert(withoutColors.material.state.vertexColorMode == RenderCore::VertexColorMode::Ignore);
    assert(withoutColors.material.state.diffuse.r == 1.0f);
    assert(withoutColors.material.state.ambient.r == 1.0f);

    NifRender::DrawableMaterialTranslation invalid;
    NifRender::initializeDrawableMaterial(invalid, true);
    const std::uint16_t invalidFlags
        = Nif::NiAlphaProperty::Flag_Blending | static_cast<std::uint16_t>(15u << 1u);
    NifRender::applyDrawableAlphaSemantics(invalidFlags, 0u, invalid);
    assert(!invalid.publishableStaticState());
    assert((invalid.issues & NifRender::drawableMaterialIssue(NifRender::DrawableMaterialIssue::InvalidPackedAlpha)) != 0);

    NifRender::DrawableMaterialTranslation dynamic;
    NifRender::initializeDrawableMaterial(dynamic, true);
    NifRender::applyLegacyMaterialSemantics(dynamic, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, 1.0f, 1.0f, true);
    assert((dynamic.issues & NifRender::drawableMaterialIssue(NifRender::DrawableMaterialIssue::DynamicMaterialController)) != 0);

    return 0;
}
