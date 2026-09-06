#ifndef OPENMW_COMPONENTS_NIFRENDER_MATERIALSEMANTICS_H
#define OPENMW_COMPONENTS_NIFRENDER_MATERIALSEMANTICS_H

#include <components/nif/property.hpp>
#include <components/rendercore/records.hpp>

#include <optional>

namespace NifRender
{
    [[nodiscard]] inline std::optional<RenderCore::BlendFactor> translateBlendFactor(int mode) noexcept
    {
        using Result = RenderCore::BlendFactor;
        switch (mode)
        {
            case 0: return Result::One;
            case 1: return Result::Zero;
            case 2: return Result::SourceColor;
            case 3: return Result::OneMinusSourceColor;
            case 4: return Result::DestinationColor;
            case 5: return Result::OneMinusDestinationColor;
            case 6: return Result::SourceAlpha;
            case 7: return Result::OneMinusSourceAlpha;
            case 8: return Result::DestinationAlpha;
            case 9: return Result::OneMinusDestinationAlpha;
            case 10: return Result::SourceAlphaSaturate;
            default: return std::nullopt;
        }
    }

    [[nodiscard]] inline std::optional<RenderCore::CompareOp> translateAlphaCompare(int mode) noexcept
    {
        using Result = RenderCore::CompareOp;
        switch (mode)
        {
            case 0: return Result::Always;
            case 1: return Result::Less;
            case 2: return Result::Equal;
            case 3: return Result::LessEqual;
            case 4: return Result::Greater;
            case 5: return Result::NotEqual;
            case 6: return Result::GreaterEqual;
            case 7: return Result::Never;
            default: return std::nullopt;
        }
    }

    [[nodiscard]] inline RenderCore::TextureApplyMode translateTextureApply(
        Nif::NiTexturingProperty::ApplyMode mode) noexcept
    {
        using Source = Nif::NiTexturingProperty::ApplyMode;
        using Result = RenderCore::TextureApplyMode;
        switch (mode)
        {
            case Source::Replace: return Result::Replace;
            case Source::Decal: return Result::Decal;
            case Source::Modulate: return Result::Modulate;
            case Source::Hilight: return Result::Highlight;
            case Source::Hilight2: return Result::Highlight2;
        }
        return Result::Modulate;
    }

    [[nodiscard]] inline RenderCore::TextureTransformConvention translateTextureTransformConvention(
        Nif::NiTextureTransform::Method method) noexcept
    {
        using Source = Nif::NiTextureTransform::Method;
        using Result = RenderCore::TextureTransformConvention;
        switch (method)
        {
            case Source::MayaLegacy: return Result::MayaLegacy;
            case Source::Max: return Result::Max;
            case Source::Maya: return Result::Maya;
        }
        return Result::Direct;
    }

    [[nodiscard]] inline RenderCore::CompareOp translateStencilCompare(Nif::NiStencilProperty::TestFunc value) noexcept
    {
        using Source = Nif::NiStencilProperty::TestFunc;
        using Result = RenderCore::CompareOp;
        switch (value)
        {
            case Source::Never: return Result::Never;
            case Source::Less: return Result::Less;
            case Source::Equal: return Result::Equal;
            case Source::LessEqual: return Result::LessEqual;
            case Source::Greater: return Result::Greater;
            case Source::NotEqual: return Result::NotEqual;
            case Source::GreaterEqual: return Result::GreaterEqual;
            case Source::Always: return Result::Always;
        }
        return Result::Never;
    }

    [[nodiscard]] inline RenderCore::StencilOp translateStencilOp(Nif::NiStencilProperty::Action value) noexcept
    {
        using Source = Nif::NiStencilProperty::Action;
        using Result = RenderCore::StencilOp;
        switch (value)
        {
            case Source::Keep: return Result::Keep;
            case Source::Zero: return Result::Zero;
            case Source::Replace: return Result::Replace;
            case Source::Increment: return Result::Increment;
            case Source::Decrement: return Result::Decrement;
            case Source::Invert: return Result::Invert;
        }
        return Result::Keep;
    }

    // Mirrors the realized V3.25 state, not the nominal file-format schema:
    // Clockwise changes front face; Both disables culling; all other draw modes
    // use counter-clockwise front faces with back-face culling enabled.
    inline void applyStencilProperty(const Nif::NiStencilProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        using DrawMode = Nif::NiStencilProperty::DrawMode;
        target.frontFace = source.mDrawMode == DrawMode::Clockwise ? RenderCore::FrontFaceWinding::Clockwise
                                                                   : RenderCore::FrontFaceWinding::CounterClockwise;
        target.cullMode = source.mDrawMode == DrawMode::Both ? RenderCore::CullMode::None : RenderCore::CullMode::Back;
        target.stencil.enabled = source.mEnabled;
        target.stencil.compare = translateStencilCompare(source.mTestFunction);
        target.stencil.reference = source.mStencilRef;
        target.stencil.compareMask = source.mStencilMask;
        target.stencil.fail = translateStencilOp(source.mFailAction);
        target.stencil.depthFail = translateStencilOp(source.mZFailAction);
        target.stencil.pass = translateStencilOp(source.mPassAction);
    }

    [[nodiscard]] inline bool applyAlphaProperty(
        const Nif::NiAlphaProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        const auto sourceBlend = translateBlendFactor(source.sourceBlendMode());
        const auto destinationBlend = translateBlendFactor(source.destinationBlendMode());
        const auto compare = translateAlphaCompare(source.alphaTestMode());
        if (!sourceBlend || !destinationBlend || !compare)
            return false;

        target.alphaBlendEnabled = source.useAlphaBlending();
        target.alphaTestEnabled = source.useAlphaTesting();
        target.sourceBlend = *sourceBlend;
        target.destinationBlend = *destinationBlend;
        target.alphaCompare = *compare;
        target.alphaCutoff = static_cast<float>(source.mThreshold) / 255.0f;
        target.transparentSort = source.noSorter() ? RenderCore::TransparentSortPolicy::Unsorted
                                                   : RenderCore::TransparentSortPolicy::Sorted;

        if (target.alphaBlendEnabled)
            target.alphaMode = RenderCore::AlphaMode::Blend;
        else if (target.alphaTestEnabled)
            target.alphaMode = RenderCore::AlphaMode::Mask;
        else
            target.alphaMode = RenderCore::AlphaMode::Opaque;
        return true;
    }

    inline void applyZBufferProperty(const Nif::NiZBufferProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        // V3.25 intentionally ignores mTestFunction and realizes only these flags.
        target.depthTest = source.depthTest();
        target.depthWrite = source.depthWrite();
    }

    inline void applyWireframeProperty(
        const Nif::NiWireframeProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        target.wireframe = source.mEnable;
    }
}

#endif
