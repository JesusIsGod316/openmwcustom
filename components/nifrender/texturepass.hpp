#ifndef OPENMW_COMPONENTS_NIFRENDER_TEXTUREPASS_H
#define OPENMW_COMPONENTS_NIFRENDER_TEXTUREPASS_H

#include "textureidentity.hpp"
#include "translationbundle.hpp"

#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace NifRender
{
    struct LegacyTextureStageSemantic
    {
        RenderCore::TextureRole role = RenderCore::TextureRole::Diffuse;
        RenderCore::TextureColorSpace colorSpace = RenderCore::TextureColorSpace::Srgb;
        RenderCore::TextureFormatClass formatClass = RenderCore::TextureFormatClass::Unknown;
    };

    // Maps only stages that V3.25's NifOsg::handleTextureProperty actually binds.
    // Raw NIF schema entries outside this set are never promoted into modern
    // semantics merely because the parser knows how to read them.
    [[nodiscard]] inline std::optional<LegacyTextureStageSemantic> legacyTextureStageSemantic(std::size_t stage) noexcept
    {
        switch (stage)
        {
            case Nif::NiTexturingProperty::BaseTexture:
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Diffuse,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Nif::NiTexturingProperty::DarkTexture:
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Dark,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Nif::NiTexturingProperty::DetailTexture:
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Detail,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Nif::NiTexturingProperty::GlossTexture:
                // V3.25 samples RGB and multiplies the environment contribution.
                // Treat it as data rather than applying color-space decoding.
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Gloss,
                    RenderCore::TextureColorSpace::Data, RenderCore::TextureFormatClass::Color };
            case Nif::NiTexturingProperty::GlowTexture:
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Emissive,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Nif::NiTexturingProperty::BumpTexture:
                // The compatibility shader consumes RG as coordinate offset and B
                // as luminance, so this is multi-channel data rather than a normal
                // map or scalar-height contract.
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Bump,
                    RenderCore::TextureColorSpace::Data, RenderCore::TextureFormatClass::Unknown };
            case Nif::NiTexturingProperty::DecalTexture:
                return LegacyTextureStageSemantic{ RenderCore::TextureRole::Decal,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            default:
                return std::nullopt;
        }
    }

    // NifOsg creates a fresh osg::Texture2D and only overrides wrap S/T. The
    // parsed NiTexturingProperty filter and anisotropy fields are not applied on
    // this current path, so preserve realized defaults instead of inventing new
    // filtering behavior in Vulkan.
    [[nodiscard]] inline RenderCore::SamplerSemantic legacyTextureSampler(bool wrapS, bool wrapT) noexcept
    {
        RenderCore::SamplerSemantic result;
        result.wrapU = wrapS ? RenderCore::TextureWrap::Repeat : RenderCore::TextureWrap::Clamp;
        result.wrapV = wrapT ? RenderCore::TextureWrap::Repeat : RenderCore::TextureWrap::Clamp;
        return result;
    }

    [[nodiscard]] inline std::optional<TranslatedTextureBinding> makeLegacyTextureBinding(TextureIndex texture,
        std::size_t stage, bool wrapS, bool wrapT, std::uint32_t uvSet) noexcept
    {
        const auto semantic = legacyTextureStageSemantic(stage);
        if (!texture.valid() || !semantic)
            return std::nullopt;

        TranslatedTextureBinding result;
        result.texture = texture;
        result.role = semantic->role;
        result.colorSpace = semantic->colorSpace;
        result.formatClass = semantic->formatClass;
        result.transform.uvSet = uvSet;
        // Current V3.25 NiTexturingProperty static transform fields are not
        // consumed by NifOsg::handleTextureProperty. Direct identity therefore
        // reproduces the realized behavior; authored transforms are diagnosed
        // below instead of becoming an accidental V4 behavior change.
        result.transform.convention = RenderCore::TextureTransformConvention::Direct;
        result.sampler = legacyTextureSampler(wrapS, wrapT);
        return result;
    }

    namespace texture_pass_detail
    {
        [[nodiscard]] inline std::optional<std::uint32_t> sourceRecordId(const Nif::Record& source) noexcept
        {
            if (source.mRecordIndex == std::numeric_limits<unsigned int>::max())
                return std::nullopt;
            return static_cast<std::uint32_t>(source.mRecordIndex);
        }

        inline void diagnose(TranslationBundle& bundle, const Nif::Record& source, DiagnosticSeverity severity,
            std::string code, std::string message)
        {
            TranslationDiagnostic diagnostic;
            diagnostic.severity = severity;
            diagnostic.sourceRecordId = sourceRecordId(source);
            diagnostic.sourceRecordType = source.mRecordName;
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);
            bundle.diagnostics.push_back(std::move(diagnostic));
        }

        [[nodiscard]] inline std::optional<TextureIndex> stageSourceTexture(const Nif::NiSourceTexture& source,
            const VFS::Manager* vfs, TranslationBundle& bundle)
        {
            const auto recordId = sourceRecordId(source);
            if (source.mExternal)
            {
                if (vfs == nullptr)
                {
                    diagnose(bundle, source, DiagnosticSeverity::Error, "texture.vfs_required",
                        "External NiSourceTexture requires the CP3B VFS-aware translation entry point");
                    return std::nullopt;
                }

                const ResolvedVfsIdentity resolved
                    = resolveTextureVfsIdentity(VFS::Path::toNormalized(source.mFile), *vfs);
                if (resolved.valid())
                    return stageResolvedTexture(bundle, resolved, recordId);

                // ImageManager deliberately substitutes its built-in magenta
                // warning image when the selected VFS resource cannot be opened.
                // Preserve that compatibility behavior as an explicit neutral
                // resource rather than failing the whole model or inventing a
                // content key from the missing path.
                diagnose(bundle, source, DiagnosticSeverity::Warning, "texture.missing_warning_fallback",
                    "External texture was not found after OpenMW path correction; using the canonical warning texture fallback");
                return stageWarningTexture(bundle, recordId);
            }

            if (!source.mData.empty())
            {
                // V3.25 supports NiPixelData, but CP3B has not yet promoted the
                // decoded/packed payload contract into backend-neutral texture
                // storage. Fail closed until that payload is represented exactly.
                diagnose(bundle, source, DiagnosticSeverity::Error, "texture.embedded_payload_pending",
                    "Embedded NiPixelData texture is supported by V3.25 but still needs an exact neutral payload representation before static publication");
                return std::nullopt;
            }

            diagnose(bundle, source, DiagnosticSeverity::Warning, "texture.empty_source",
                "NiSourceTexture has neither an external VFS path nor embedded pixel data; V3.25 realizes an empty texture object");
            return std::nullopt;
        }
    }

    // Fold one inherited NiTexturingProperty into an already-created neutral
    // material. Each property replaces all bindings inherited from an earlier
    // NiTexturingProperty, matching NifOsg::clearBoundTextures exactly.
    inline void applyLegacyTextureProperty(const Nif::NiTexturingProperty& source, const VFS::Manager* vfs,
        TranslatedMaterial& material, TranslationBundle& bundle)
    {
        material.textures.clear();
        material.state.textureApply = translateTextureApply(source.mApplyMode);
        material.supplement.bumpMapMatrix = { source.mBumpMapMatrix.x(), source.mBumpMapMatrix.y(),
            source.mBumpMapMatrix.z(), source.mBumpMapMatrix.w() };
        material.supplement.environmentMapLumaBias = { source.mEnvMapLumaBias.x(), source.mEnvMapLumaBias.y() };

        const bool hasControllerChain = !source.mController.empty();
        if (hasControllerChain)
        {
            texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Info, "texture.controller_deferred",
                "Texture controller chain is recognized; static base bindings are retained where available and dynamic replacement remains deferred to CP3D");
        }

        for (std::size_t stage = 0; stage < source.mTextures.size(); ++stage)
        {
            const Nif::NiTexturingProperty::Texture& texture = source.mTextures[stage];
            const bool controllerOnlyBase
                = stage == Nif::NiTexturingProperty::BaseTexture && hasControllerChain && !texture.mEnabled;
            if (!texture.mEnabled && !controllerOnlyBase)
                continue;

            if (!legacyTextureStageSemantic(stage))
            {
                texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Warning,
                    "texture.stage_unsupported",
                    "Enabled NiTexturingProperty stage is not bound by the current V3.25 renderer and will not be invented by the neutral translator");
                continue;
            }

            if (texture.mHasTransform)
            {
                texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Info,
                    "texture.static_transform_v325_ignored",
                    "Authored NiTexturingProperty static texture transform is not consumed by the current V3.25 NifOsg path; neutral static binding keeps the realized identity transform");
            }

            if (controllerOnlyBase)
            {
                // NifOsg installs a repeat/UV0 null placeholder so an active
                // NiFlipController has a target texture unit. There is no stable
                // image identity to publish until controller playback exists.
                texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Info,
                    "texture.flip_placeholder_deferred",
                    "Base texture is controller-only; V3.25 creates a null repeat/UV0 placeholder and dynamic image selection is deferred to CP3D");
                continue;
            }

            if (texture.mSourceTexture.empty())
            {
                texture_pass_detail::diagnose(bundle, source,
                    stage == Nif::NiTexturingProperty::BaseTexture ? DiagnosticSeverity::Warning
                                                                  : DiagnosticSeverity::Info,
                    "texture.enabled_source_missing",
                    "Enabled texture slot has no NiSourceTexture; current V3.25 leaves that image binding empty");
                continue;
            }

            const std::optional<TextureIndex> staged = texture_pass_detail::stageSourceTexture(
                *texture.mSourceTexture.getPtr(), vfs, bundle);
            if (!staged)
                continue;

            const auto binding = makeLegacyTextureBinding(
                *staged, stage, texture.wrapS(), texture.wrapT(), texture.mUVSet);
            if (!binding)
            {
                texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Error,
                    "texture.binding_semantic_missing",
                    "Resolved texture could not be mapped to a supported neutral binding semantic");
                continue;
            }
            material.textures.push_back(*binding);
        }

        bool hasShaderTexture = false;
        for (const auto& texture : source.mShaderTextures)
            hasShaderTexture = hasShaderTexture || texture.mEnabled;
        if (hasShaderTexture)
        {
            texture_pass_detail::diagnose(bundle, source, DiagnosticSeverity::Info,
                "texture.shader_slots_v325_ignored",
                "NiTexturingProperty shader-texture slots are parsed but not bound by the current V3.25 NifOsg texture-property path");
        }
    }
}

#endif
