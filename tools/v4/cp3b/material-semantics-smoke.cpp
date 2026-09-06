#include <components/nifrender/materialsemantics.hpp>

#include <cassert>

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

    Nif::NiAlphaProperty alpha;
    alpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing
        | Nif::NiAlphaProperty::Flag_NoSorter | static_cast<std::uint16_t>(6u << 1u)
        | static_cast<std::uint16_t>(7u << 5u) | static_cast<std::uint16_t>(4u << 10u);
    alpha.mThreshold = 64u;

    RenderCore::MaterialRecord material;
    assert(applyAlphaProperty(alpha, material));
    assert(material.alphaBlendEnabled);
    assert(material.alphaTestEnabled);
    assert(material.alphaMode == RenderCore::AlphaMode::Blend);
    assert(material.sourceBlend == RenderCore::BlendFactor::SourceAlpha);
    assert(material.destinationBlend == RenderCore::BlendFactor::OneMinusSourceAlpha);
    assert(material.alphaCompare == RenderCore::CompareOp::Greater);
    assert(material.transparentSort == RenderCore::TransparentSortPolicy::Unsorted);
    assert(material.alphaCutoff > 0.250f && material.alphaCutoff < 0.252f);

    Nif::NiStencilProperty stencil;
    stencil.mEnabled = true;
    stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::GreaterEqual;
    stencil.mStencilRef = 3u;
    stencil.mStencilMask = 0xffu;
    stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
    stencil.mZFailAction = Nif::NiStencilProperty::Action::Increment;
    stencil.mPassAction = Nif::NiStencilProperty::Action::Replace;
    stencil.mDrawMode = Nif::NiStencilProperty::DrawMode::Clockwise;
    applyStencilProperty(stencil, material);
    assert(material.frontFace == RenderCore::FrontFaceWinding::Clockwise);
    assert(material.cullMode == RenderCore::CullMode::Back);
    assert(material.stencil.enabled);
    assert(material.stencil.compare == RenderCore::CompareOp::GreaterEqual);
    assert(material.stencil.depthFail == RenderCore::StencilOp::Increment);

    stencil.mDrawMode = Nif::NiStencilProperty::DrawMode::Both;
    applyStencilProperty(stencil, material);
    assert(material.frontFace == RenderCore::FrontFaceWinding::CounterClockwise);
    assert(material.cullMode == RenderCore::CullMode::None);

    Nif::NiZBufferProperty depth;
    depth.mFlags = 2u;
    applyZBufferProperty(depth, material);
    assert(!material.depthTest);
    assert(material.depthWrite);

    Nif::NiWireframeProperty wire;
    wire.mEnable = true;
    applyWireframeProperty(wire, material);
    assert(material.wireframe);

    return 0;
}
