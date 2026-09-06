#include <components/nifrender/materialsemantics.hpp>

#include <cassert>
#include <cstdint>

int main()
{
    using namespace NifRender;

    for (int mode = 0; mode <= 10; ++mode)
        assert(translateBlendFactor(mode));
    assert(!translateBlendFactor(11));

    assert(translateAlphaCompare(0) == RenderCore::CompareOp::Always);
    assert(translateAlphaCompare(3) == RenderCore::CompareOp::LessEqual);
    assert(translateAlphaCompare(7) == RenderCore::CompareOp::Never);
    assert(!translateAlphaCompare(8));

    assert(translateTextureApply(Nif::NiTexturingProperty::ApplyMode::Replace)
        == RenderCore::TextureApplyMode::Replace);
    assert(translateTextureApply(Nif::NiTexturingProperty::ApplyMode::Hilight2)
        == RenderCore::TextureApplyMode::Highlight2);
    assert(translateTextureTransformConvention(Nif::NiTextureTransform::Method::MayaLegacy)
        == RenderCore::TextureTransformConvention::MayaLegacy);
    assert(translateTextureTransformConvention(Nif::NiTextureTransform::Method::Max)
        == RenderCore::TextureTransformConvention::Max);
    assert(translateTextureTransformConvention(Nif::NiTextureTransform::Method::Maya)
        == RenderCore::TextureTransformConvention::Maya);

    const std::uint16_t alphaFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing
        | Nif::NiAlphaProperty::Flag_NoSorter | static_cast<std::uint16_t>(6u << 1u)
        | static_cast<std::uint16_t>(7u << 5u) | static_cast<std::uint16_t>(4u << 10u);

    RenderCore::MaterialRecord material;
    assert(applyAlphaSemantics(alphaFlags, 64u, material));
    assert(material.alphaBlendEnabled);
    assert(material.alphaTestEnabled);
    assert(material.alphaMode == RenderCore::AlphaMode::Blend);
    assert(material.sourceBlend == RenderCore::BlendFactor::SourceAlpha);
    assert(material.destinationBlend == RenderCore::BlendFactor::OneMinusSourceAlpha);
    assert(material.alphaCompare == RenderCore::CompareOp::Greater);
    assert(material.transparentSort == RenderCore::TransparentSortPolicy::Unsorted);
    assert(material.alphaCutoff > 0.250f && material.alphaCutoff < 0.252f);

    applyStencilSemantics(true, Nif::NiStencilProperty::TestFunc::GreaterEqual, 3u, 0xffu,
        Nif::NiStencilProperty::Action::Keep, Nif::NiStencilProperty::Action::Increment,
        Nif::NiStencilProperty::Action::Replace, Nif::NiStencilProperty::DrawMode::Clockwise, material);
    assert(material.frontFace == RenderCore::FrontFaceWinding::Clockwise);
    assert(material.cullMode == RenderCore::CullMode::Back);
    assert(material.stencil.enabled);
    assert(material.stencil.compare == RenderCore::CompareOp::GreaterEqual);
    assert(material.stencil.depthFail == RenderCore::StencilOp::Increment);

    applyStencilSemantics(true, Nif::NiStencilProperty::TestFunc::GreaterEqual, 3u, 0xffu,
        Nif::NiStencilProperty::Action::Keep, Nif::NiStencilProperty::Action::Increment,
        Nif::NiStencilProperty::Action::Replace, Nif::NiStencilProperty::DrawMode::Both, material);
    assert(material.frontFace == RenderCore::FrontFaceWinding::CounterClockwise);
    assert(material.cullMode == RenderCore::CullMode::None);

    applyZBufferSemantics(2u, material);
    assert(!material.depthTest);
    assert(material.depthWrite);

    applyWireframeSemantics(true, material);
    assert(material.wireframe);

    return 0;
}
