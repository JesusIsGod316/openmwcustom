#ifndef OPENMW_COMPONENTS_NIFRENDER_MATERIALPASS_H
#define OPENMW_COMPONENTS_NIFRENDER_MATERIALPASS_H

#include "drawablematerial.hpp"
#include "texturepass.hpp"
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
            const VFS::Manager* vfs, TranslatedMaterial& material, TranslationBundle& bundle)
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
                        applyLegacyTextureProperty(
                            *static_cast<const Nif::NiTexturingProperty*>(property), vfs, material, bundle);
                        break;
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

        inline void normalizeTextureUvSets(const Nif::NiAVObject& node, const TranslatedModelNode& targetNode,
            TranslatedMaterial& material, TranslationBundle& bundle)
        {
            if (!targetNode.mesh || !targetNode.mesh->valid() || targetNode.mesh->value() >= bundle.meshes.size())
                return;
            const RenderCore::MeshRecord& mesh = bundle.meshes[targetNode.mesh->value()].record;
            if (!mesh.payload)
                return;

            const std::size_t uvSetCount = mesh.payload->texCoordSets.size();
            for (TranslatedTextureBinding& binding : material.textures)
            {
                if (binding.transform.uvSet < uvSetCount)
                    continue;

                if (uvSetCount == 0u)
                {
                    // NifOsg keeps the image binding but installs no texcoord array
                    // when the geometry has no UV sets. Preserve that ownership and
                    // normalize the otherwise unusable index for deterministic backends.
                    binding.transform.uvSet = 0u;
                    diagnose(bundle, node, DiagnosticSeverity::Warning, "texture.uv_stream_missing",
                        "Texture is bound to geometry with no UV stream; V3.25 keeps the binding but supplies no texture-coordinate array");
                    continue;
                }

                // Current V3.25 logs and falls back to UV set zero when the authored
                // set index is out of range and at least one set exists.
                binding.transform.uvSet = 0u;
                diagnose(bundle, node, DiagnosticSeverity::Warning, "texture.uv_set_fallback_zero",
                    "Authored texture UV set is out of range for this geometry; reproducing V3.25 fallback to UV set zero");
            }
        }

        class MaterialPass final
        {
        public:
            MaterialPass(Nif::FileView file, TranslationBundle& bundle, const VFS::Manager* vfs)
                : mFile(file)
                , mBundle(bundle)
                , mVfs(vfs)
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
                applyInheritedNodeState(inheritedState, mVfs, translated.material, mBundle);
                normalizeTextureUvSets(node, targetNode, translated.material, mBundle);
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
                        "External shader material remains explicit and will be resolved by the next CP3B2 shader-material VFS slice");

                const MaterialIndex materialIndex{ static_cast<std::uint32_t>(mBundle.materials.size()) };
                mBundle.materials.push_back(std::move(translated.material));
                targetNode.materials.push_back(materialIndex);
            }

            Nif::FileView mFile;
            TranslationBundle& mBundle;
            const VFS::Manager* mVfs = nullptr;
            std::unordered_map<std::uint32_t, TranslatedModelNode*> mGeometryNodes;
        };
    }

    // Structural/source-only entry point retained for focused translator tests.
    // If an external texture is encountered, the texture pass emits an Error so
    // this path can never be used for lossy final static publication.
    inline void applyStaticMaterialPass(Nif::FileView file, TranslationBundle& bundle)
    {
        material_pass_detail::MaterialPass(file, bundle, nullptr).run();
    }

    // Production CP3B2 material path: consumes the already parsed FileView and
    // the live VFS index, preserving current OpenMW path correction, winning
    // archive selection and content identity without introducing a second parser.
    inline void applyStaticMaterialPass(Nif::FileView file, const VFS::Manager& vfs, TranslationBundle& bundle)
    {
        material_pass_detail::MaterialPass(file, bundle, &vfs).run();
    }
}

#endif
