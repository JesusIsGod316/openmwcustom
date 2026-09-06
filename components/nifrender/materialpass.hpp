#ifndef OPENMW_COMPONENTS_NIFRENDER_MATERIALPASS_H
#define OPENMW_COMPONENTS_NIFRENDER_MATERIALPASS_H

#include "drawablematerial.hpp"
#include "translationbundle.hpp"

#include <components/nif/data.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NifRender
{
    namespace material_pass_detail
    {
        [[nodiscard]] inline bool isDrawableFoldProperty(Nif::RecordType type) noexcept
        {
            switch (type)
            {
                case Nif::RC_NiMaterialProperty:
                case Nif::RC_NiVertexColorProperty:
                case Nif::RC_NiSpecularProperty:
                case Nif::RC_NiAlphaProperty:
                    return true;
                default:
                    return false;
            }
        }

        inline void diagnose(TranslationBundle& bundle, const Nif::Record& source, DiagnosticSeverity severity,
            std::string code, std::string message)
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

        inline void applyInheritedNodeState(const std::vector<const Nif::NiProperty*>& properties,
            TranslatedMaterial& material, TranslationBundle& bundle)
        {
            for (const Nif::NiProperty* property : properties)
            {
                if (property == nullptr)
                    continue;

                switch (property->mRecordType)
                {
                    case Nif::RC_NiStencilProperty:
                        applyStencilProperty(*static_cast<const Nif::NiStencilProperty*>(property), material.state);
                        break;
                    case Nif::RC_NiWireframeProperty:
                        applyWireframeProperty(*static_cast<const Nif::NiWireframeProperty*>(property), material.state);
                        break;
                    case Nif::RC_NiZBufferProperty:
                        applyZBufferProperty(*static_cast<const Nif::NiZBufferProperty*>(property), material.state);
                        break;
                    case Nif::RC_NiTexturingProperty:
                    {
                        const auto& source = *static_cast<const Nif::NiTexturingProperty*>(property);
                        material.state.textureApply = translateTextureApply(source.mApplyMode);
                        material.supplement.bumpMapMatrix = {
                            source.mBumpMapMatrix.x(), source.mBumpMapMatrix.y(), source.mBumpMapMatrix.z(), source.mBumpMapMatrix.w() };
                        material.supplement.environmentMapLumaBias = {
                            source.mEnvMapLumaBias.x(), source.mEnvMapLumaBias.y() };
                        // Texture bindings themselves are resolved by the VFS-aware
                        // CP3B2 texture pass. Record the deferral once per property.
                        bool hasEnabledTexture = false;
                        for (const auto& texture : source.mTextures)
                            hasEnabledTexture = hasEnabledTexture || texture.mEnabled;
                        if (hasEnabledTexture)
                            diagnose(bundle, source, DiagnosticSeverity::Info, "material.textures_vfs_deferred",
                                "Texture property state is preserved; VFS-aware image identity/binding is deferred to the CP3B2 texture pass");
                        break;
                    }
                    case Nif::RC_NiFogProperty:
                    {
                        const auto& source = *static_cast<const Nif::NiFogProperty*>(property);
                        // Match the realized V3.25 behavior exactly: vertex-alpha fog
                        // is treated as broken and disables fog; otherwise enabled fog
                        // overrides color/depth. The radial source bit is not honored by
                        // the current renderer and therefore is not invented here.
                        const bool enabled = source.enabled() && !source.vertexAlpha();
                        material.supplement.fog.mode = enabled ? MaterialFogMode::Override : MaterialFogMode::Disabled;
                        material.supplement.fog.depth = source.mFogDepth;
                        material.supplement.fog.color = {
                            source.mColour.x(), source.mColour.y(), source.mColour.z(), 1.0f };
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        class MaterialPass final
        {
        public:
            MaterialPass(Nif::FileView file, TranslationBundle& bundle)
                : mFile(file)
                , mBundle(bundle)
            {
                for (std::size_t i = 0; i < bundle.model.nodes.size(); ++i)
                {
                    auto& node = bundle.model.nodes[i];
                    if (node.sourceRecordId && node.kind == RenderCore::ModelNodeKind::Geometry)
                        mGeometryNodes.emplace(*node.sourceRecordId, &node);
                }
            }

            void run()
            {
                for (std::size_t rootIndex = 0; rootIndex < mFile.numRoots(); ++rootIndex)
                {
                    const Nif::Record* record = mFile.getRoot(rootIndex);
                    const auto* root = dynamic_cast<const Nif::NiAVObject*>(record);
                    if (root != nullptr)
                        walk(*root, {}, {});
                }
            }

        private:
            void walk(const Nif::NiAVObject& node, std::vector<const Nif::NiProperty*> drawableProperties,
                std::vector<const Nif::NiProperty*> inheritedState)
            {
                for (const auto& property : node.mProperties)
                {
                    if (property.empty())
                        continue;
                    inheritedState.push_back(property.getPtr());
                    if (isDrawableFoldProperty(property->mRecordType))
                        drawableProperties.push_back(property.getPtr());
                }

                const auto found = mGeometryNodes.find(static_cast<std::uint32_t>(node.mRecordIndex));
                if (found != mGeometryNodes.end())
                    translateGeometry(node, drawableProperties, inheritedState, *found->second);

                const auto* group = dynamic_cast<const Nif::NiNode*>(&node);
                if (group == nullptr)
                    return;
                for (const Nif::NiAVObjectPtr& child : group->mChildren)
                {
                    if (!child.empty())
                        walk(*child.getPtr(), drawableProperties, inheritedState);
                }
            }

            void translateGeometry(const Nif::NiAVObject& node, std::vector<const Nif::NiProperty*> drawableProperties,
                const std::vector<const Nif::NiProperty*>& inheritedState, TranslatedModelNode& targetNode)
            {
                bool hasVertexColors = false;
                if (const auto* legacy = dynamic_cast<const Nif::NiGeometry*>(&node))
                {
                    if (!legacy->mData.empty())
                        hasVertexColors = !legacy->mData->mColors.empty();
                    if (!legacy->mShaderProperty.empty())
                        drawableProperties.push_back(legacy->mShaderProperty.getPtr());
                    if (!legacy->mAlphaProperty.empty())
                        drawableProperties.push_back(legacy->mAlphaProperty.getPtr());
                }
                else if (const auto* bethesda = dynamic_cast<const Nif::BSTriShape*>(&node))
                {
                    hasVertexColors = (bethesda->mVertDesc.mFlags & Nif::BSVertexDesc::VertexAttribute::Vertex_Colors) != 0;
                    if (!bethesda->mShaderProperty.empty())
                        drawableProperties.push_back(bethesda->mShaderProperty.getPtr());
                    if (!bethesda->mAlphaProperty.empty())
                        drawableProperties.push_back(bethesda->mAlphaProperty.getPtr());
                }

                DrawableMaterialTranslation translated
                    = translateDrawableMaterial(drawableProperties, hasVertexColors, mFile.getVersion());
                applyInheritedNodeState(inheritedState, translated.material, mBundle);
                translated.material.state.sourceIdentity
                    = mBundle.sourceIdentity + "#material:" + std::to_string(static_cast<unsigned int>(node.mRecordIndex));

                if ((translated.issues & drawableMaterialIssue(DrawableMaterialIssue::InvalidPackedAlpha)) != 0)
                    diagnose(mBundle, node, DiagnosticSeverity::Error, "material.invalid_packed_alpha",
                        "Drawable contains an unknown packed alpha mode; static publication must fail closed");
                if ((translated.issues & drawableMaterialIssue(DrawableMaterialIssue::DynamicMaterialController)) != 0)
                    diagnose(mBundle, node, DiagnosticSeverity::Info, "material.controller_deferred",
                        "Static base material is retained; dynamic material controller playback remains deferred to CP3D");
                if ((translated.issues & drawableMaterialIssue(DrawableMaterialIssue::ExternalShaderMaterial)) != 0)
                    diagnose(mBundle, node, DiagnosticSeverity::Info, "material.external_shader_vfs_deferred",
                        "External shader material requires VFS-aware resolution in CP3B2 before final static publication");

                const MaterialIndex materialIndex{ static_cast<std::uint32_t>(mBundle.materials.size()) };
                mBundle.materials.push_back(std::move(translated.material));
                targetNode.materials.push_back(materialIndex);
            }

            Nif::FileView mFile;
            TranslationBundle& mBundle;
            std::unordered_map<std::uint32_t, TranslatedModelNode*> mGeometryNodes;
        };
    }

    // Completes the CP3B1 material fold over the geometry/hierarchy translator
    // without reparsing source bytes. The operation is deterministic and only
    // consumes the existing FileView plus stable source record IDs.
    inline void applyStaticMaterialPass(Nif::FileView file, TranslationBundle& bundle)
    {
        material_pass_detail::MaterialPass(file, bundle).run();
    }
}

#endif
