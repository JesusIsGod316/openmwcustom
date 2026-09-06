#include <components/nifrender/drawablematerial.hpp>

#include <cassert>
#include <vector>

int main()
{
    Nif::NiMaterialProperty parentMaterial;
    parentMaterial.mRecordType = Nif::RC_NiMaterialProperty;
    parentMaterial.mDiffuse.set(0.2f, 0.3f, 0.4f);
    parentMaterial.mAmbient.set(0.1f, 0.2f, 0.3f);
    parentMaterial.mSpecular.set(0.8f, 0.7f, 0.6f);
    parentMaterial.mEmissive.set(0.05f, 0.04f, 0.03f);
    parentMaterial.mGlossiness = 42.0f;
    parentMaterial.mAlpha = 0.75f;

    Nif::NiMaterialProperty childMaterial;
    childMaterial.mRecordType = Nif::RC_NiMaterialProperty;
    childMaterial.mDiffuse.set(0.6f, 0.5f, 0.4f);
    childMaterial.mAmbient.set(0.3f, 0.2f, 0.1f);
    childMaterial.mSpecular.set(0.9f, 0.8f, 0.7f);
    childMaterial.mEmissive.set(0.2f, 0.1f, 0.0f);
    childMaterial.mGlossiness = 64.0f;
    childMaterial.mAlpha = 0.5f;

    Nif::NiVertexColorProperty vertexColor;
    vertexColor.mRecordType = Nif::RC_NiVertexColorProperty;
    vertexColor.mVertexMode = Nif::NiVertexColorProperty::VertexMode::VertMode_SrcAmbDif;
    vertexColor.mLightingMode = Nif::NiVertexColorProperty::LightMode::LightMode_EmiAmbDif;

    std::vector<const Nif::NiProperty*> properties{ &parentMaterial, &childMaterial, &vertexColor };
    auto translated = NifRender::translateDrawableMaterial(properties, true, Nif::NIFFile::VER_OB);
    assert(translated.publishableStaticState());
    assert(translated.material.state.diffuse.r == 0.6f);
    assert(translated.material.state.diffuse.a == 0.5f);
    assert(translated.material.state.shininess == 64.0f);
    assert(translated.material.state.vertexColorMode == RenderCore::VertexColorMode::AmbientDiffuse);

    auto mw = NifRender::translateDrawableMaterial(properties, true, Nif::NIFFile::VER_MW);
    assert(mw.material.state.specular.r == 0.0f);
    assert(mw.material.state.shininess == 0.0f);

    auto withoutColors = NifRender::translateDrawableMaterial(properties, false, Nif::NIFFile::VER_OB);
    assert(withoutColors.material.state.vertexColorMode == RenderCore::VertexColorMode::Ignore);
    assert(withoutColors.material.state.diffuse.r == 1.0f);
    assert(withoutColors.material.state.ambient.r == 1.0f);

    Nif::NiAlphaProperty invalidAlpha;
    invalidAlpha.mRecordType = Nif::RC_NiAlphaProperty;
    invalidAlpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | static_cast<std::uint16_t>(15u << 1u);
    properties.push_back(&invalidAlpha);
    auto invalid = NifRender::translateDrawableMaterial(properties, true, Nif::NIFFile::VER_OB);
    assert(!invalid.publishableStaticState());
    assert((invalid.issues & NifRender::drawableMaterialIssue(NifRender::DrawableMaterialIssue::InvalidPackedAlpha)) != 0);

    return 0;
}
