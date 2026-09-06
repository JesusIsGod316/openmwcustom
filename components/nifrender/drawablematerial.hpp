#ifndef OPENMW_COMPONENTS_NIFRENDER_DRAWABLEMATERIAL_H
#define OPENMW_COMPONENTS_NIFRENDER_DRAWABLEMATERIAL_H

#include "materialsemantics.hpp"
#include "translationbundle.hpp"

#include <components/nif/niffile.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace NifRender
{
    enum class DrawableMaterialIssue : std::uint32_t
    {
        None = 0,
        InvalidPackedAlpha = 1u << 0,
        DynamicMaterialController = 1u << 1,
        ExternalShaderMaterial = 1u << 2,
    };

    [[nodiscard]] constexpr std::uint32_t drawableMaterialIssue(DrawableMaterialIssue issue) noexcept
    {
        return static_cast<std::uint32_t>(issue);
    }

    struct DrawableMaterialTranslation
    {
        TranslatedMaterial material;
        std::uint32_t issues = 0;

        [[nodiscard]] bool publishableStaticState() const noexcept
        {
            return (issues & drawableMaterialIssue(DrawableMaterialIssue::InvalidPackedAlpha)) == 0;
        }
    };

    inline void initializeDrawableMaterial(DrawableMaterialTranslation& result, bool hasVertexColors) noexcept
    {
        RenderCore::MaterialRecord& material = result.material.state;
        // Current drawable defaults differ from graphics API defaults.
        material.diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
        material.ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
        material.vertexColorMode = hasVertexColors ? RenderCore::VertexColorMode::AmbientDiffuse
                                                   : RenderCore::VertexColorMode::Ignore;
    }

    // Value-level helpers are deliberately parser-object-free. The production
    // property fold below remains the NIF adapter seam, while semantic QC can
    // exercise ordering and fallback rules without linking parser record vtables.
    inline void applyLegacyMaterialSemantics(DrawableMaterialTranslation& result, const glm::vec3& diffuse,
        const glm::vec3& ambient, const glm::vec3& specular, const glm::vec3& emissive, float glossiness, float alpha,
        float emissiveMultiplier, bool hasDynamicController) noexcept
    {
        RenderCore::MaterialRecord& material = result.material.state;
        material.diffuse = { diffuse.x, diffuse.y, diffuse.z, alpha };
        material.ambient = { ambient.x, ambient.y, ambient.z, 1.0f };
        material.specular = { specular.x, specular.y, specular.z, 1.0f };
        material.emission = { emissive.x, emissive.y, emissive.z, 1.0f };
        material.shininess = glossiness;
        material.emissiveMultiplier = emissiveMultiplier;
        material.alpha = alpha;
        if (hasDynamicController)
            result.issues |= drawableMaterialIssue(DrawableMaterialIssue::DynamicMaterialController);
    }

    inline void applyVertexColorSemantics(Nif::NiVertexColorProperty::VertexMode vertexMode,
        Nif::NiVertexColorProperty::LightMode lightingMode, RenderCore::MaterialRecord& material,
        Nif::NiVertexColorProperty::LightMode& currentLightMode) noexcept
    {
        using VertexMode = Nif::NiVertexColorProperty::VertexMode;
        using LightMode = Nif::NiVertexColorProperty::LightMode;
        switch (vertexMode)
        {
            case VertexMode::VertMode_SrcIgnore:
                material.vertexColorMode = RenderCore::VertexColorMode::Ignore;
                break;
            case VertexMode::VertMode_SrcEmissive:
                material.vertexColorMode = RenderCore::VertexColorMode::Emissive;
                break;
            case VertexMode::VertMode_SrcAmbDif:
                currentLightMode = lightingMode;
                material.vertexColorMode = currentLightMode == LightMode::LightMode_Emissive
                    ? RenderCore::VertexColorMode::Ignore
                    : RenderCore::VertexColorMode::AmbientDiffuse;
                break;
        }
    }

    inline void applyDrawableAlphaSemantics(
        std::uint16_t flags, std::uint8_t threshold, DrawableMaterialTranslation& result) noexcept
    {
        if (!applyAlphaSemantics(flags, threshold, result.material.state))
            result.issues |= drawableMaterialIssue(DrawableMaterialIssue::InvalidPackedAlpha);
    }

    inline void finalizeDrawableMaterial(DrawableMaterialTranslation& result, bool hasVertexColors,
        unsigned int nifVersion, bool specularEnabled, Nif::NiVertexColorProperty::LightMode lightMode) noexcept
    {
        RenderCore::MaterialRecord& material = result.material.state;

        // Morrowind disables specular support even when the file carries nominal
        // specular state. Later formats also honor NiSpecularProperty disabling.
        if (nifVersion <= Nif::NIFFile::VER_MW || !specularEnabled)
        {
            material.specular = { 0.0f, 0.0f, 0.0f, 0.0f };
            material.shininess = 0.0f;
            material.specularStrength = 1.0f;
        }

        if (lightMode == Nif::NiVertexColorProperty::LightMode::LightMode_Emissive)
        {
            material.diffuse = { 0.0f, 0.0f, 0.0f, material.diffuse.a };
            material.ambient = { 0.0f, 0.0f, 0.0f, 0.0f };
        }

        // V3.25 falls back to neutral material colors when a property requests
        // vertex colors but the mesh has no color stream, then disables vertex
        // color interpretation for the drawable.
        if (!hasVertexColors)
        {
            if (material.vertexColorMode == RenderCore::VertexColorMode::AmbientDiffuse)
            {
                material.ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
                material.diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
            }
            else if (material.vertexColorMode == RenderCore::VertexColorMode::Emissive)
                material.emission = { 1.0f, 1.0f, 1.0f, 1.0f };
            material.vertexColorMode = RenderCore::VertexColorMode::Ignore;
        }
    }

    // Reproduces the drawable-level property folding performed by V3.25 after
    // inherited properties have been collected from root to leaf. Later entries
    // therefore override earlier entries exactly where the current renderer does.
    [[nodiscard]] inline DrawableMaterialTranslation translateDrawableMaterial(
        const std::vector<const Nif::NiProperty*>& properties, bool hasVertexColors, unsigned int nifVersion)
    {
        DrawableMaterialTranslation result;
        initializeDrawableMaterial(result, hasVertexColors);
        RenderCore::MaterialRecord& material = result.material.state;

        bool specularEnabled = true;
        auto lightMode = Nif::NiVertexColorProperty::LightMode::LightMode_EmiAmbDif;

        for (const Nif::NiProperty* property : properties)
        {
            if (property == nullptr)
                continue;

            switch (property->mRecordType)
            {
                case Nif::RC_NiSpecularProperty:
                    specularEnabled = static_cast<const Nif::NiSpecularProperty*>(property)->mEnable;
                    break;

                case Nif::RC_NiMaterialProperty:
                {
                    const auto& source = *static_cast<const Nif::NiMaterialProperty*>(property);
                    applyLegacyMaterialSemantics(result, { source.mDiffuse.x(), source.mDiffuse.y(), source.mDiffuse.z() },
                        { source.mAmbient.x(), source.mAmbient.y(), source.mAmbient.z() },
                        { source.mSpecular.x(), source.mSpecular.y(), source.mSpecular.z() },
                        { source.mEmissive.x(), source.mEmissive.y(), source.mEmissive.z() }, source.mGlossiness,
                        source.mAlpha, source.mEmissiveMult, !source.mController.empty());
                    break;
                }

                case Nif::RC_NiVertexColorProperty:
                {
                    const auto& source = *static_cast<const Nif::NiVertexColorProperty*>(property);
                    applyVertexColorSemantics(source.mVertexMode, source.mLightingMode, material, lightMode);
                    break;
                }

                case Nif::RC_NiAlphaProperty:
                {
                    const auto& source = *static_cast<const Nif::NiAlphaProperty*>(property);
                    applyDrawableAlphaSemantics(source.mFlags, source.mThreshold, result);
                    break;
                }

                case Nif::RC_BSShaderPPLightingProperty:
                    specularEnabled = static_cast<const Nif::BSShaderPPLightingProperty*>(property)->specular();
                    break;

                case Nif::RC_BSLightingShaderProperty:
                {
                    const auto& source = *static_cast<const Nif::BSLightingShaderProperty*>(property);
                    if (!source.mName.empty())
                        result.issues |= drawableMaterialIssue(DrawableMaterialIssue::ExternalShaderMaterial);
                    material.alpha = source.mAlpha;
                    material.emission = { source.mEmissive.x(), source.mEmissive.y(), source.mEmissive.z(), 1.0f };
                    material.specular = { source.mSpecular.x(), source.mSpecular.y(), source.mSpecular.z(), 1.0f };
                    material.shininess = source.mGlossiness;
                    material.emissiveMultiplier = source.mEmissiveMult;
                    material.specularStrength = source.mSpecStrength;
                    specularEnabled = source.specular();
                    result.material.supplement.decal = source.decal();
                    result.material.supplement.treeAnimation = source.treeAnim();
                    result.material.supplement.refraction = source.refraction();
                    result.material.supplement.refractionStrength = source.mRefractionStrength;
                    if (source.doubleSided())
                        material.cullMode = RenderCore::CullMode::None;
                    material.depthTest = source.depthTest();
                    material.depthWrite = source.depthWrite();
                    break;
                }

                case Nif::RC_BSEffectShaderProperty:
                {
                    const auto& source = *static_cast<const Nif::BSEffectShaderProperty*>(property);
                    if (!source.mName.empty())
                        result.issues |= drawableMaterialIssue(DrawableMaterialIssue::ExternalShaderMaterial);
                    result.material.supplement.decal = source.decal();
                    result.material.supplement.treeAnimation = source.treeAnim();
                    result.material.supplement.refraction = source.refraction();
                    result.material.supplement.refractionStrength = source.mRefractionPower;
                    result.material.supplement.softEffect = source.softEffect();
                    result.material.supplement.softEffectDepth = source.mFalloffDepth;
                    result.material.supplement.falloff = source.useFalloff();
                    result.material.supplement.falloffParams = {
                        source.mFalloffParams.x(), source.mFalloffParams.y(), source.mFalloffParams.z(), source.mFalloffParams.w() };
                    if (source.doubleSided())
                        material.cullMode = RenderCore::CullMode::None;
                    material.depthTest = source.depthTest();
                    material.depthWrite = source.depthWrite();
                    break;
                }

                default:
                    break;
            }
        }

        finalizeDrawableMaterial(result, hasVertexColors, nifVersion, specularEnabled, lightMode);
        return result;
    }
}

#endif
