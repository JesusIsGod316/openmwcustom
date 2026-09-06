#ifndef OPENMW_COMPONENTS_NIFRENDER_MATERIALSEMANTICS_H
#define OPENMW_COMPONENTS_NIFRENDER_MATERIALSEMANTICS_H

#include <components/nif/property.hpp>
#include <components/rendercore/records.hpp>

#include <cstdint>
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

    // The raw-value helpers keep semantic mapping independently testable without
    // constructing parser records whose virtual read/post functions require the
    // complete NIF parser linkage. Property wrappers below remain the production seam.
    inline void applyStencilSemantics(bool enabled, Nif::NiStencilProperty::TestFunc testFunction,
        std::uint32_t stencilReference, std::uint32_t stencilMask, Nif::NiStencilProperty::Action failAction,
        Nif::NiStencilProperty::Action depthFailAction, Nif::NiStencilProperty::Action passAction,
        Nif::NiStencilProperty::DrawMode drawMode, RenderCore::MaterialRecord& target) noexcept
    {
        using DrawMode = Nif::NiStencilProperty::DrawMode;
        target.frontFace = drawMode == DrawMode::Clockwise ? RenderCore::FrontFaceWinding::Clockwise
                                                            : RenderCore::FrontFaceWinding::CounterClockwise;
        target.cullMode = drawMode == DrawMode::Both ? RenderCore::CullMode::None : RenderCore::CullMode::Back;
        target.stencil.enabled = enabled;
        target.stencil.compare = translateStencilCompare(testFunction);
        target.stencil.reference = stencilReference;
        target.stencil.compareMask = stencilMask;
        target.stencil.fail = translateStencilOp(failAction);
        target.stencil.depthFail = translateStencilOp(depthFailAction);
        target.stencil.pass = translateStencilOp(passAction);
    }

    // Mirrors the realized V3.25 state, not the nominal file-format schema:
    // Clockwise changes front face; Both disables culling; all other draw modes
    // use counter-clockwise front faces with back-face culling enabled.
    inline void applyStencilProperty(const Nif::NiStencilProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        applyStencilSemantics(source.mEnabled, source.mTestFunction, source.mStencilRef, source.mStencilMask,
            source.mFailAction, source.mZFailAction, source.mPassAction, source.mDrawMode, target);
    }

    [[nodiscard]] inline bool applyAlphaSemantics(
        std::uint16_t flags, std::uint8_t threshold, RenderCore::MaterialRecord& target) noexcept
    {
        const int sourceMode = static_cast<int>((flags >> 1u) & 0x0fu);
        const int destinationMode = static_cast<int>((flags >> 5u) & 0x0fu);
        const int testMode = static_cast<int>((flags >> 10u) & 0x07u);
        const auto sourceBlend = translateBlendFactor(sourceMode);
        const auto destinationBlend = translateBlendFactor(destinationMode);
        const auto compare = translateAlphaCompare(testMode);
        if (!sourceBlend || !destinationBlend || !compare)
            return false;

        target.alphaBlendEnabled = (flags & Nif::NiAlphaProperty::Flag_Blending) != 0u;
        target.alphaTestEnabled = (flags & Nif::NiAlphaProperty::Flag_Testing) != 0u;
        target.sourceBlend = *sourceBlend;
        target.destinationBlend = *destinationBlend;
        target.alphaCompare = *compare;
        target.alphaCutoff = static_cast<float>(threshold) / 255.0f;
        target.transparentSort = (flags & Nif::NiAlphaProperty::Flag_NoSorter) != 0u
            ? RenderCore::TransparentSortPolicy::Unsorted
            : RenderCore::TransparentSortPolicy::Sorted;

        if (target.alphaBlendEnabled)
            target.alphaMode = RenderCore::AlphaMode::Blend;
        else if (target.alphaTestEnabled)
            target.alphaMode = RenderCore::AlphaMode::Mask;
        else
            target.alphaMode = RenderCore::AlphaMode::Opaque;
        return true;
    }

    [[nodiscard]] inline bool applyAlphaProperty(
        const Nif::NiAlphaProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        return applyAlphaSemantics(source.mFlags, source.mThreshold, target);
    }

    inline void applyZBufferSemantics(std::uint16_t flags, RenderCore::MaterialRecord& target) noexcept
    {
        // V3.25 intentionally ignores the nominal comparison function and realizes
        // only the legacy bit-0 depth-test and bit-1 depth-write behavior.
        target.depthTest = (flags & 0x0001u) != 0u;
        target.depthWrite = (flags & 0x0002u) != 0u;
    }

    inline void applyZBufferProperty(const Nif::NiZBufferProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        applyZBufferSemantics(source.mFlags, target);
    }

    inline void applyWireframeSemantics(bool enabled, RenderCore::MaterialRecord& target) noexcept
    {
        target.wireframe = enabled;
    }

    inline void applyWireframeProperty(
        const Nif::NiWireframeProperty& source, RenderCore::MaterialRecord& target) noexcept
    {
        applyWireframeSemantics(source.mEnable, target);
    }
}

#endif
