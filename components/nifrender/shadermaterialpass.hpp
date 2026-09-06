#ifndef OPENMW_COMPONENTS_NIFRENDER_SHADERMATERIALPASS_H
#define OPENMW_COMPONENTS_NIFRENDER_SHADERMATERIALPASS_H

#include "drawablematerial.hpp"
#include "textureidentity.hpp"
#include "texturepass.hpp"
#include "translationbundle.hpp"

#include <components/bgsm/file.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NifRender
{
    enum class ShaderMaterialKind : std::uint8_t
    {
        Lighting,
        Effect,
    };

    struct ShaderTextureInterpretation
    {
        RenderCore::TextureRole role = RenderCore::TextureRole::Diffuse;
        RenderCore::TextureColorSpace colorSpace = RenderCore::TextureColorSpace::Srgb;
        RenderCore::TextureFormatClass formatClass = RenderCore::TextureFormatClass::Color;
    };

    struct ShaderTextureSource
    {
        std::string authoredPath;
        ShaderTextureInterpretation interpretation;
    };

    // Parser-independent value object containing only BGSM/BGEM state that
    // current V3.25 actually realizes on this NifOsg path. File-format fields
    // current OpenMW does not consume are deliberately not promoted into a new
    // backend contract merely because the schema exposes them.
    struct ShaderMaterialSemantic
    {
        ShaderMaterialKind kind = ShaderMaterialKind::Lighting;
        float transparency = 1.0f;
        bool alphaBlend = false;
        std::uint32_t sourceBlendMode = 6u;
        std::uint32_t destinationBlendMode = 7u;
        bool alphaTest = false;
        std::uint8_t alphaTestThreshold = 0u;
        bool decal = false;
        bool twoSided = false;
        bool depthTest = true;
        bool depthWrite = true;
        bool wrapS = true;
        bool wrapT = true;
        RenderCore::Color emission{ 0.0f, 0.0f, 0.0f, 1.0f };
        float emissiveMultiplier = 1.0f;
        bool treeAnimation = false;
        bool falloff = false;
        glm::vec4 falloffParams{ 0.0f };
        bool softEffect = false;
        float softEffectDepth = 0.0f;
        std::vector<ShaderTextureSource> textures;
    };

    struct ResolvedShaderMaterial
    {
        VFS::Path::Normalized canonicalPath;
        ShaderMaterialSemantic semantic;
    };

    [[nodiscard]] inline std::optional<ShaderTextureInterpretation> bsTextureSetStageSemantic(
        std::size_t stage) noexcept
    {
        using Type = Nif::BSShaderTextureSet::TextureType;
        switch (static_cast<Type>(stage))
        {
            case Type::Base:
                return ShaderTextureInterpretation{ RenderCore::TextureRole::Diffuse,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Type::Normal:
                return ShaderTextureInterpretation{ RenderCore::TextureRole::Normal,
                    RenderCore::TextureColorSpace::Data, RenderCore::TextureFormatClass::Normal };
            case Type::Glow:
                return ShaderTextureInterpretation{ RenderCore::TextureRole::Emissive,
                    RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
            case Type::Parallax:
            case Type::Environment:
            case Type::EnvironmentMask:
            case Type::Subsurface:
            case Type::BackLighting:
                return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline RenderCore::TextureTransform makeBsEffectTextureTransform(
        const glm::vec2& authoredOffset, const glm::vec2& authoredScale) noexcept
    {
        RenderCore::TextureTransform result;
        // V3.25 builds T(center) * S * T(-center), then adds -mUVOffset.
        // Store the realized convention directly so every backend does not have
        // to rediscover the source-field sign quirk.
        result.offset = -authoredOffset;
        result.scale = authoredScale;
        result.center = { 0.5f, 0.5f };
        result.rotation = 0.0f;
        result.uvSet = 0u;
        result.convention = RenderCore::TextureTransformConvention::Direct;
        return result;
    }

    inline void resetShaderSupplement(MaterialSupplement& supplement) noexcept
    {
        supplement.decal = false;
        supplement.treeAnimation = false;
        supplement.refraction = false;
        supplement.refractionStrength = 0.0f;
        supplement.softEffect = false;
        supplement.softEffectDepth = 0.0f;
        supplement.falloff = false;
        supplement.falloffParams = glm::vec4(0.0f);
    }

    // External shader material semantics replace the inline BS shader-property
    // fields when BGSM/BGEM resolves. MaterialPass replays the geometry-local
    // NiAlphaProperty afterwards so source ordering remains identical to V3.25.
    [[nodiscard]] inline bool applyShaderMaterialSemantic(
        const ShaderMaterialSemantic& source, DrawableMaterialTranslation& target) noexcept
    {
        RenderCore::MaterialRecord& material = target.material.state;
        resetShaderSupplement(target.material.supplement);

        material.alpha = source.transparency;
        material.alphaBlendEnabled = source.alphaBlend;
        material.alphaTestEnabled = source.alphaTest;
        material.alphaCompare = source.alphaTest ? RenderCore::CompareOp::Greater : RenderCore::CompareOp::Always;
        material.alphaCutoff = static_cast<float>(source.alphaTestThreshold) / 255.0f;
        material.transparentSort = source.alphaBlend ? RenderCore::TransparentSortPolicy::Sorted
                                                     : RenderCore::TransparentSortPolicy::Inherit;

        if (source.alphaBlend)
        {
            const auto src = translateBlendFactor(static_cast<int>(source.sourceBlendMode));
            const auto dst = translateBlendFactor(static_cast<int>(source.destinationBlendMode));
            if (!src || !dst)
            {
                target.issues |= drawableMaterialIssue(DrawableMaterialIssue::InvalidPackedAlpha);
                return false;
            }
            material.sourceBlend = *src;
            material.destinationBlend = *dst;
        }

        if (source.alphaBlend)
            material.alphaMode = RenderCore::AlphaMode::Blend;
        else if (source.alphaTest)
            material.alphaMode = RenderCore::AlphaMode::Mask;
        else
            material.alphaMode = RenderCore::AlphaMode::Opaque;

        target.material.supplement.decal = source.decal;
        material.emission = source.emission;

        if (source.kind == ShaderMaterialKind::Lighting)
        {
            // Current V3.25 explicitly disables BGSM specular in this path; the
            // source implementation carries a TODO for future PBR specular.
            material.specular = { 0.0f, 0.0f, 0.0f, 0.0f };
            material.shininess = 0.0f;
            material.specularStrength = 1.0f;
            material.emissiveMultiplier = source.emissiveMultiplier;
            target.material.supplement.treeAnimation = source.treeAnimation;
        }
        else
        {
            target.material.supplement.falloff = source.falloff;
            target.material.supplement.falloffParams = source.falloffParams;
            target.material.supplement.softEffect = source.softEffect;
            target.material.supplement.softEffectDepth = source.softEffectDepth;
        }

        // Clear inline special-shader state before inherited node state is
        // folded. The special shader node state is replayed after inheritance.
        material.cullMode = RenderCore::CullMode::Back;
        material.depthTest = source.depthTest;
        material.depthWrite = source.depthWrite;
        target.issues &= ~drawableMaterialIssue(DrawableMaterialIssue::ExternalShaderMaterial);
        return true;
    }

    [[nodiscard]] inline ShaderMaterialSemantic makeShaderMaterialSemantic(const Bgsm::MaterialFile& source)
    {
        ShaderMaterialSemantic result;
        result.transparency = source.mTransparency;
        result.alphaBlend = source.mAlphaBlend;
        result.sourceBlendMode = source.mSourceBlendMode;
        result.destinationBlendMode = source.mDestinationBlendMode;
        result.alphaTest = source.mAlphaTest;
        result.alphaTestThreshold = source.mAlphaTestThreshold;
        result.decal = source.mDecal;
        result.twoSided = source.mTwoSided;
        result.depthTest = source.mDepthTest;
        result.depthWrite = source.mDepthWrite;
        result.wrapS = source.wrapS();
        result.wrapT = source.wrapT();

        if (source.mShaderType == Bgsm::ShaderType::Lighting)
        {
            result.kind = ShaderMaterialKind::Lighting;
            const auto& lighting = static_cast<const Bgsm::BGSMFile&>(source);
            result.emission = { lighting.mEmittanceColor.x(), lighting.mEmittanceColor.y(),
                lighting.mEmittanceColor.z(), 1.0f };
            result.emissiveMultiplier = lighting.mEmittanceMult;
            result.treeAnimation = lighting.mTree;

            if (!lighting.mDiffuseMap.empty())
                result.textures.push_back({ lighting.mDiffuseMap,
                    { RenderCore::TextureRole::Diffuse, RenderCore::TextureColorSpace::Srgb,
                        RenderCore::TextureFormatClass::Color } });
            if (!lighting.mNormalMap.empty())
                result.textures.push_back({ lighting.mNormalMap,
                    { RenderCore::TextureRole::Normal, RenderCore::TextureColorSpace::Data,
                        RenderCore::TextureFormatClass::Normal } });
            if (lighting.mGlowMapEnabled && !lighting.mGlowMap.empty())
                result.textures.push_back({ lighting.mGlowMap,
                    { RenderCore::TextureRole::Emissive, RenderCore::TextureColorSpace::Srgb,
                        RenderCore::TextureFormatClass::Color } });
        }
        else
        {
            result.kind = ShaderMaterialKind::Effect;
            const auto& effect = static_cast<const Bgsm::BGEMFile&>(source);
            result.emission
                = { effect.mEmittanceColor.x(), effect.mEmittanceColor.y(), effect.mEmittanceColor.z(), 1.0f };
            result.falloff = effect.mFalloff;
            result.falloffParams = { effect.mFalloffParams.x(), effect.mFalloffParams.y(),
                effect.mFalloffParams.z(), effect.mFalloffParams.w() };
            result.softEffect = effect.mSoft;
            result.softEffectDepth = effect.mSoftDepth;

            // Current V3.25's BGEM node path binds only the base map.
            if (!effect.mBaseMap.empty())
                result.textures.push_back({ effect.mBaseMap,
                    { RenderCore::TextureRole::Diffuse, RenderCore::TextureColorSpace::Srgb,
                        RenderCore::TextureFormatClass::Color } });
        }
        return result;
    }

    [[nodiscard]] inline bool isExternalShaderMaterialReference(const Nif::BSShaderProperty& source)
    {
        if (source.mName.empty())
            return false;
        const VFS::Path::Normalized path = VFS::Path::toNormalized(source.mName);
        return path.value().ends_with(".bgsm") || path.value().ends_with(".bgem");
    }

    inline void shaderMaterialDiagnostic(TranslationBundle& bundle, const Nif::Record& source,
        DiagnosticSeverity severity, std::string code, std::string message)
    {
        TranslationDiagnostic diagnostic;
        diagnostic.severity = severity;
        if (source.mRecordIndex != std::numeric_limits<unsigned int>::max())
            diagnostic.sourceRecordId = static_cast<std::uint32_t>(source.mRecordIndex);
        diagnostic.sourceRecordType = source.mRecordName;
        diagnostic.code = std::move(code);
        diagnostic.message = std::move(message);
        bundle.diagnostics.push_back(std::move(diagnostic));
    }

    [[nodiscard]] inline std::optional<ResolvedShaderMaterial> resolveExternalShaderMaterial(
        const Nif::BSShaderProperty& source, const VFS::Manager* vfs, TranslationBundle& bundle)
    {
        if (!isExternalShaderMaterialReference(source))
            return std::nullopt;
        if (vfs == nullptr)
        {
            shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Error, "shader_material.vfs_required",
                "BGSM/BGEM reference requires the VFS-aware static translation entry; source-only translation cannot publish it");
            return std::nullopt;
        }

        const VFS::Path::Normalized authored = VFS::Path::toNormalized(source.mName);
        const VFS::Path::Normalized corrected = Misc::ResourceHelpers::correctMaterialPath(authored, *vfs);
        try
        {
            auto stream = vfs->find(corrected);
            if (!stream)
            {
                shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Warning,
                    "shader_material.inline_fallback_missing",
                    "BGSM/BGEM material was not found in the winning VFS view; reproducing V3.25 inline shader-property fallback");
                return std::nullopt;
            }

            Bgsm::MaterialFilePtr parsed = Bgsm::parse(std::move(stream));
            if (!parsed)
            {
                shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Warning,
                    "shader_material.inline_fallback_empty",
                    "BGSM/BGEM parser returned no material; reproducing V3.25 inline shader-property fallback");
                return std::nullopt;
            }

            ResolvedShaderMaterial result;
            result.canonicalPath = corrected;
            result.semantic = makeShaderMaterialSemantic(*parsed);
            return result;
        }
        catch (const std::exception& exception)
        {
            shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Warning,
                "shader_material.inline_fallback_parse",
                std::string("BGSM/BGEM material failed to parse; reproducing V3.25 inline shader-property fallback: ")
                    + exception.what());
            return std::nullopt;
        }
    }

    [[nodiscard]] inline TextureIndex stageShaderTexture(const std::string& authoredPath, const VFS::Manager* vfs,
        TranslationBundle& bundle, const Nif::Record& source)
    {
        const std::optional<std::uint32_t> sourceRecordId
            = source.mRecordIndex == std::numeric_limits<unsigned int>::max()
            ? std::nullopt
            : std::optional<std::uint32_t>(static_cast<std::uint32_t>(source.mRecordIndex));

        if (vfs == nullptr)
        {
            shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Error, "shader_texture.vfs_required",
                "BS shader texture requires the VFS-aware static translation entry");
            return {};
        }

        const ResolvedVfsIdentity resolved
            = resolveTextureVfsIdentity(VFS::Path::toNormalized(authoredPath), *vfs);
        if (resolved.valid())
            return stageResolvedTexture(bundle, resolved, sourceRecordId);

        shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Warning, "shader_texture.warning_fallback",
            "BS shader texture is missing from the winning VFS view; reproducing the shared V3.25 warning-image fallback");
        return stageWarningTexture(bundle, sourceRecordId);
    }

    inline void appendShaderTextureBinding(TranslatedMaterial& material, TextureIndex texture,
        const ShaderTextureInterpretation& interpretation, bool wrapS, bool wrapT,
        RenderCore::TextureTransform transform = {})
    {
        if (!texture.valid())
            return;
        TranslatedTextureBinding binding;
        binding.texture = texture;
        binding.role = interpretation.role;
        binding.colorSpace = interpretation.colorSpace;
        binding.formatClass = interpretation.formatClass;
        binding.transform = transform;
        binding.sampler = legacyTextureSampler(wrapS, wrapT);
        material.textures.push_back(std::move(binding));
    }

    inline void applyExternalShaderMaterialNodeState(const ResolvedShaderMaterial& resolved,
        const Nif::BSShaderProperty& source, const VFS::Manager* vfs, TranslatedMaterial& material,
        TranslationBundle& bundle)
    {
        material.textures.clear();
        for (const ShaderTextureSource& texture : resolved.semantic.textures)
        {
            const TextureIndex index = stageShaderTexture(texture.authoredPath, vfs, bundle, source);
            appendShaderTextureBinding(
                material, index, texture.interpretation, resolved.semantic.wrapS, resolved.semantic.wrapT);
        }

        // Special shader node state is applied after inherited state in V3.25.
        if (resolved.semantic.twoSided)
            material.state.cullMode = RenderCore::CullMode::None;
        material.state.depthTest = resolved.semantic.depthTest;
        material.state.depthWrite = resolved.semantic.depthWrite;
        material.supplement.treeAnimation = resolved.semantic.treeAnimation;
        material.supplement.falloff = resolved.semantic.falloff;
        material.supplement.falloffParams = resolved.semantic.falloffParams;
        material.supplement.softEffect = resolved.semantic.softEffect;
        material.supplement.softEffectDepth = resolved.semantic.softEffectDepth;
    }

    inline void applyBsTextureSet(const Nif::BSShaderTextureSet& textureSet, bool wrapS, bool wrapT,
        const Nif::Record& source, const VFS::Manager* vfs, TranslatedMaterial& material, TranslationBundle& bundle)
    {
        for (std::size_t stage = 0; stage < textureSet.mTextures.size(); ++stage)
        {
            const std::string& authoredPath = textureSet.mTextures[stage];
            if (authoredPath.empty())
                continue;

            const auto interpretation = bsTextureSetStageSemantic(stage);
            if (!interpretation)
            {
                shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Info, "shader_texture.stage_v325_ignored",
                    "BS shader texture-set stage is populated but current V3.25 does not bind that stage");
                continue;
            }

            const TextureIndex texture = stageShaderTexture(authoredPath, vfs, bundle, source);
            appendShaderTextureBinding(material, texture, *interpretation, wrapS, wrapT);
        }
    }

    inline void applyInlineBsShaderNodeState(const Nif::BSShaderProperty& source, const VFS::Manager* vfs,
        TranslatedMaterial& material, TranslationBundle& bundle)
    {
        switch (source.mRecordType)
        {
            case Nif::RC_BSShaderPPLightingProperty:
            {
                const auto& property = static_cast<const Nif::BSShaderPPLightingProperty&>(source);
                material.textures.clear();
                if (!property.mTextureSet.empty())
                    applyBsTextureSet(*property.mTextureSet.getPtr(), property.wrapS(), property.wrapT(), source, vfs,
                        material, bundle);
                break;
            }
            case Nif::RC_BSLightingShaderProperty:
            {
                const auto& property = static_cast<const Nif::BSLightingShaderProperty&>(source);
                material.textures.clear();
                if (!property.mTextureSet.empty())
                    applyBsTextureSet(*property.mTextureSet.getPtr(), property.wrapS(), property.wrapT(), source, vfs,
                        material, bundle);
                if (property.doubleSided())
                    material.state.cullMode = RenderCore::CullMode::None;
                material.state.depthTest = property.depthTest();
                material.state.depthWrite = property.depthWrite();
                material.supplement.treeAnimation = property.treeAnim();
                material.supplement.refraction = property.refraction();
                material.supplement.refractionStrength = property.mRefractionStrength;
                break;
            }
            case Nif::RC_BSEffectShaderProperty:
            {
                const auto& property = static_cast<const Nif::BSEffectShaderProperty&>(source);
                material.textures.clear();
                if (!property.mSourceTexture.empty())
                {
                    const ShaderTextureInterpretation interpretation{ RenderCore::TextureRole::Diffuse,
                        RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
                    const TextureIndex texture = stageShaderTexture(property.mSourceTexture, vfs, bundle, source);
                    appendShaderTextureBinding(material, texture, interpretation, property.wrapS(), property.wrapT(),
                        makeBsEffectTextureTransform({ property.mUVOffset.x(), property.mUVOffset.y() },
                            { property.mUVScale.x(), property.mUVScale.y() }));
                }

                if (!property.mGreyscaleTexture.empty() || !property.mEnvMapTexture.empty()
                    || !property.mNormalTexture.empty() || !property.mEnvMaskTexture.empty()
                    || !property.mReflectanceTexture.empty() || !property.mLightingTexture.empty()
                    || !property.mEmitGradientTexture.empty())
                {
                    shaderMaterialDiagnostic(bundle, source, DiagnosticSeverity::Info,
                        "shader_texture.effect_aux_v325_ignored",
                        "BSEffect auxiliary texture fields are populated but current V3.25 binds only mSourceTexture on this path");
                }

                if (property.doubleSided())
                    material.state.cullMode = RenderCore::CullMode::None;
                material.state.depthTest = property.depthTest();
                material.state.depthWrite = property.depthWrite();
                material.supplement.treeAnimation = property.treeAnim();
                material.supplement.refraction = property.refraction();
                material.supplement.refractionStrength = property.mRefractionPower;
                material.supplement.softEffect = property.softEffect();
                material.supplement.softEffectDepth = property.mFalloffDepth;
                material.supplement.falloff = property.useFalloff();
                material.supplement.falloffParams = { property.mFalloffParams.x(), property.mFalloffParams.y(),
                    property.mFalloffParams.z(), property.mFalloffParams.w() };
                break;
            }
            case Nif::RC_BSShaderNoLightingProperty:
            {
                const auto& property = static_cast<const Nif::BSShaderNoLightingProperty&>(source);
                material.textures.clear();
                if (!property.mFilename.empty())
                {
                    const ShaderTextureInterpretation interpretation{ RenderCore::TextureRole::Diffuse,
                        RenderCore::TextureColorSpace::Srgb, RenderCore::TextureFormatClass::Color };
                    const TextureIndex texture = stageShaderTexture(property.mFilename, vfs, bundle, source);
                    appendShaderTextureBinding(material, texture, interpretation, property.wrapS(), property.wrapT());
                }
                break;
            }
            default:
                break;
        }
    }
}

#endif
