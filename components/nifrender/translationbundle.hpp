#ifndef OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONBUNDLE_H
#define OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONBUNDLE_H

#include <components/rendercore/records.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace NifRender
{
    template <class Tag>
    class LocalIndex final
    {
    public:
        constexpr LocalIndex() noexcept = default;
        explicit constexpr LocalIndex(std::uint32_t value) noexcept
            : mValue(value)
        {
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return mValue != std::numeric_limits<std::uint32_t>::max();
        }
        explicit constexpr operator bool() const noexcept { return valid(); }
        [[nodiscard]] constexpr std::uint32_t value() const noexcept { return mValue; }

        friend constexpr bool operator==(LocalIndex, LocalIndex) noexcept = default;

    private:
        std::uint32_t mValue = std::numeric_limits<std::uint32_t>::max();
    };

    struct TextureIndexTag final
    {
    };
    struct MaterialIndexTag final
    {
    };
    struct MeshIndexTag final
    {
    };

    using TextureIndex = LocalIndex<TextureIndexTag>;
    using MaterialIndex = LocalIndex<MaterialIndexTag>;
    using MeshIndex = LocalIndex<MeshIndexTag>;

    enum class TranslationDisposition : std::uint8_t
    {
        Rendered,
        CollisionOnly,
        Hidden,
        Deferred,
        Unsupported,
        Ignored,
    };

    enum class DiagnosticSeverity : std::uint8_t
    {
        Info,
        Warning,
        Error,
    };

    // Exactly one outcome is emitted for each source record the translator
    // deliberately classifies. Diagnostics are independent and may be many per
    // record; CP3B4 accounting therefore never depends on log-message count.
    struct TranslationOutcome
    {
        TranslationDisposition disposition = TranslationDisposition::Ignored;
        std::optional<std::uint32_t> sourceRecordId;
        std::string sourceRecordType;
    };

    struct TranslationDiagnostic
    {
        DiagnosticSeverity severity = DiagnosticSeverity::Info;
        std::optional<std::uint32_t> sourceRecordId;
        std::string sourceRecordType;
        std::string code;
        std::string message;
    };

    struct TranslationSummary
    {
        std::uint32_t rendered = 0;
        std::uint32_t collisionOnly = 0;
        std::uint32_t hidden = 0;
        std::uint32_t deferred = 0;
        std::uint32_t unsupported = 0;
        std::uint32_t ignored = 0;

        void observe(TranslationDisposition disposition) noexcept
        {
            switch (disposition)
            {
                case TranslationDisposition::Rendered:
                    ++rendered;
                    break;
                case TranslationDisposition::CollisionOnly:
                    ++collisionOnly;
                    break;
                case TranslationDisposition::Hidden:
                    ++hidden;
                    break;
                case TranslationDisposition::Deferred:
                    ++deferred;
                    break;
                case TranslationDisposition::Unsupported:
                    ++unsupported;
                    break;
                case TranslationDisposition::Ignored:
                    ++ignored;
                    break;
            }
        }
    };

    enum class TextureStorage : std::uint8_t
    {
        ExternalVfs,
        EmbeddedNif,
        WarningFallback,
    };

    struct TranslatedTexture
    {
        RenderCore::TextureRecord record;
        TextureStorage storage = TextureStorage::ExternalVfs;
        std::optional<std::uint32_t> sourceRecordId;
    };

    struct TranslatedTextureBinding
    {
        TextureIndex texture;
        RenderCore::TextureRole role = RenderCore::TextureRole::Diffuse;
        RenderCore::TextureColorSpace colorSpace = RenderCore::TextureColorSpace::Srgb;
        RenderCore::TextureFormatClass formatClass = RenderCore::TextureFormatClass::Unknown;
        RenderCore::TextureTransform transform;
        RenderCore::SamplerSemantic sampler;
    };

    // Keep the translator-facing names source-compatible while the actual fog
    // vocabulary now lives in the stable backend-neutral RenderCore contract.
    using MaterialFogMode = RenderCore::MaterialFogMode;
    using MaterialFogSemantic = RenderCore::MaterialFogSemantic;

    // Translation staging keeps source-folded semantics separate from
    // MaterialRecord only until local TextureIndex values are deterministically
    // rebound to stable TextureHandles. publishTranslation promotes every field
    // below into the stable RenderCore MaterialRecord; this is no longer a
    // publication blocker or a backend-private side channel.
    struct MaterialSupplement
    {
        bool decal = false;
        bool hasBumpParameters = false;
        glm::vec4 bumpMapMatrix{ 1.0f, 0.0f, 0.0f, 1.0f };
        glm::vec2 environmentMapLumaBias{ 0.0f, 0.0f };
        MaterialFogSemantic fog;
        bool treeAnimation = false;
        bool refraction = false;
        float refractionStrength = 0.0f;
        bool softEffect = false;
        float softEffectDepth = 0.0f;
        bool falloff = false;
        glm::vec4 falloffParams{ 0.0f };
    };

    struct TranslatedMaterial
    {
        // Static state is already expressed in the backend-neutral RenderCore
        // vocabulary. The texture vector must remain empty until deterministic
        // binding maps local texture indices to published TextureHandles.
        RenderCore::MaterialRecord state;
        std::vector<TranslatedTextureBinding> textures;
        MaterialSupplement supplement;
    };

    inline void promoteMaterialSupplement(const TranslatedMaterial& source, RenderCore::MaterialRecord& target) noexcept
    {
        target.decal = source.supplement.decal;

        // V3.25 installs bump uniforms only when the bump stage is actually
        // realized. Derive that fact from the staged binding as well as the
        // explicit flag so a static bump binding cannot silently lose its
        // matrix/luma semantics while controller-only dynamic cases stay deferred.
        target.bumpParametersEnabled = source.supplement.hasBumpParameters;
        if (!target.bumpParametersEnabled)
        {
            for (const TranslatedTextureBinding& binding : source.textures)
            {
                if (binding.role == RenderCore::TextureRole::Bump)
                {
                    target.bumpParametersEnabled = true;
                    break;
                }
            }
        }
        if (target.bumpParametersEnabled)
        {
            target.bumpMapMatrix = source.supplement.bumpMapMatrix;
            target.environmentMapLumaBias = source.supplement.environmentMapLumaBias;
        }
        else
        {
            target.bumpMapMatrix = { 1.0f, 0.0f, 0.0f, 1.0f };
            target.environmentMapLumaBias = { 0.0f, 0.0f };
        }

        target.fog = source.supplement.fog;
        target.treeAnimation = source.supplement.treeAnimation;
        target.refraction = source.supplement.refraction;
        target.refractionStrength = source.supplement.refractionStrength;
        target.softEffect = source.supplement.softEffect;
        target.softEffectDepth = source.supplement.softEffectDepth;
        target.falloff = source.supplement.falloff;
        target.falloffParams = source.supplement.falloffParams;
    }

    struct TranslatedMesh
    {
        RenderCore::MeshRecord record;
        std::optional<std::uint32_t> sourceRecordId;
    };

    struct TranslatedModelNode
    {
        std::string name;
        std::optional<std::uint32_t> sourceRecordId;
        RenderCore::ModelNodeIndex parent;

        // NIF NiTransform matrices may contain negative and non-uniform scale
        // components. Preserve the authored affine transform exactly here;
        // translation must never depend on lossy quaternion decomposition.
        glm::mat4 localTransform{ 1.0f };

        RenderCore::ModelNodeKind kind = RenderCore::ModelNodeKind::Transform;
        std::optional<MeshIndex> mesh;
        std::vector<MaterialIndex> materials;
        std::optional<RenderCore::ModelNodeIndex> activeSwitchChild;
        std::optional<RenderCore::ModelLodSemantic> lod;
        std::optional<RenderCore::ModelBillboardMode> billboard;
        std::optional<RenderCore::ModelSortSemantic> sort;
        std::uint32_t flags = 0;
    };

    struct TranslatedModel
    {
        std::string sourceIdentity;
        std::string contentIdentity;
        RenderCore::AxisAlignedBounds bounds;
        std::vector<TranslatedModelNode> nodes;
        std::vector<RenderCore::ModelNodeIndex> roots;
    };

    namespace detail
    {
        [[nodiscard]] inline bool finite(float value) noexcept { return std::isfinite(value); }

        [[nodiscard]] inline bool finite(const glm::vec2& value) noexcept
        {
            return finite(value.x) && finite(value.y);
        }

        [[nodiscard]] inline bool finite(const glm::vec3& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] inline bool finite(const glm::vec4& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
        }

        [[nodiscard]] inline bool finite(const glm::mat4& value) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    if (!finite(value[column][row]))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] inline bool validTextureTransform(const RenderCore::TextureTransform& value) noexcept
        {
            return finite(value.offset) && finite(value.scale) && finite(value.center) && finite(value.rotation);
        }

        [[nodiscard]] inline bool validMaterialState(const RenderCore::MaterialRecord& value) noexcept
        {
            return value.revision.valid() && value.textures.empty() && finite(value.diffuse) && finite(value.ambient)
                && finite(value.specular) && finite(value.emission) && finite(value.environmentMapColor)
                && finite(value.shininess) && finite(value.emissiveMultiplier) && finite(value.specularStrength)
                && finite(value.environmentMapStrength) && finite(value.alpha) && finite(value.alphaCutoff)
                && finite(value.bumpMapMatrix) && finite(value.environmentMapLumaBias) && finite(value.fog.color)
                && finite(value.fog.depth) && finite(value.refractionStrength) && finite(value.softEffectDepth)
                && finite(value.falloffParams);
        }

        [[nodiscard]] inline bool validMaterialSupplement(const MaterialSupplement& value) noexcept
        {
            return finite(value.bumpMapMatrix) && finite(value.environmentMapLumaBias) && finite(value.fog.color)
                && finite(value.fog.depth) && finite(value.refractionStrength) && finite(value.softEffectDepth)
                && finite(value.falloffParams);
        }
    }

    struct TranslationBundle
    {
        std::string sourceIdentity;
        std::string contentIdentity;
        std::vector<TranslatedTexture> textures;
        std::vector<TranslatedMaterial> materials;
        std::vector<TranslatedMesh> meshes;
        TranslatedModel model;
        std::vector<TranslationOutcome> outcomes;
        std::vector<TranslationDiagnostic> diagnostics;

        [[nodiscard]] TranslationSummary summary() const noexcept
        {
            TranslationSummary result;
            for (const TranslationOutcome& outcome : outcomes)
                result.observe(outcome.disposition);
            return result;
        }

        [[nodiscard]] bool hasErrors() const noexcept
        {
            for (const TranslationDiagnostic& diagnostic : diagnostics)
            {
                if (diagnostic.severity == DiagnosticSeverity::Error)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            for (const TranslatedTexture& texture : textures)
            {
                if (!texture.record.revision.valid() || texture.record.contentIdentity.empty())
                    return false;
                if (texture.storage == TextureStorage::ExternalVfs && texture.record.sourceIdentity.empty())
                    return false;
                if (texture.storage == TextureStorage::WarningFallback
                    && texture.record.contentIdentity != "builtin:openmw-warning-image-v1")
                    return false;
            }

            for (const TranslatedMaterial& material : materials)
            {
                if (!detail::validMaterialState(material.state) || !detail::validMaterialSupplement(material.supplement))
                    return false;
                for (const TranslatedTextureBinding& binding : material.textures)
                {
                    if (!binding.texture.valid() || binding.texture.value() >= textures.size()
                        || !detail::validTextureTransform(binding.transform)
                        || !detail::finite(binding.sampler.maxAnisotropy))
                        return false;
                }
            }

            for (const TranslatedMesh& mesh : meshes)
            {
                if (!mesh.record.revision.valid())
                    return false;
                if (mesh.record.payload)
                {
                    if (!RenderCore::validMeshPayload(*mesh.record.payload)
                        || mesh.record.surfaceCount != mesh.record.payload->surfaces.size())
                        return false;
                }
            }

            std::vector<RenderCore::ModelNodeIndex> expectedRoots;
            std::vector<std::uint32_t> childCounts(model.nodes.size(), 0);
            for (std::size_t i = 0; i < model.nodes.size(); ++i)
            {
                const TranslatedModelNode& node = model.nodes[i];
                if (!detail::finite(node.localTransform))
                    return false;

                if (node.parent.valid())
                {
                    if (node.parent.value() >= i)
                        return false;
                    ++childCounts[node.parent.value()];
                }
                else
                    expectedRoots.emplace_back(static_cast<std::uint32_t>(i));

                if (node.mesh && (!node.mesh->valid() || node.mesh->value() >= meshes.size()))
                    return false;
                for (const MaterialIndex material : node.materials)
                {
                    if (!material.valid() || material.value() >= materials.size())
                        return false;
                }
                if ((node.flags & RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::CollisionOnly)) != 0
                    && (node.flags & RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Collision)) == 0)
                    return false;
            }

            if (model.roots != expectedRoots)
                return false;

            for (std::size_t i = 0; i < model.nodes.size(); ++i)
            {
                const TranslatedModelNode& node = model.nodes[i];
                const auto isDirectChild = [&](RenderCore::ModelNodeIndex child) {
                    return child.valid() && child.value() < model.nodes.size()
                        && model.nodes[child.value()].parent
                            == RenderCore::ModelNodeIndex{ static_cast<std::uint32_t>(i) };
                };

                if ((node.kind == RenderCore::ModelNodeKind::Geometry) != node.mesh.has_value())
                    return false;

                if (node.kind == RenderCore::ModelNodeKind::Switch)
                {
                    if (node.activeSwitchChild && !isDirectChild(*node.activeSwitchChild))
                        return false;
                }
                else if (node.activeSwitchChild)
                    return false;

                if (node.kind == RenderCore::ModelNodeKind::Lod)
                {
                    if (!node.lod || !detail::finite(node.lod->center) || node.lod->ranges.size() != childCounts[i])
                        return false;
                    for (std::size_t rangeIndex = 0; rangeIndex < node.lod->ranges.size(); ++rangeIndex)
                    {
                        const RenderCore::ModelLodRange& range = node.lod->ranges[rangeIndex];
                        if (!isDirectChild(range.child) || !detail::finite(range.minimumDistance)
                            || !detail::finite(range.maximumDistance) || range.minimumDistance > range.maximumDistance)
                            return false;
                        for (std::size_t other = rangeIndex + 1; other < node.lod->ranges.size(); ++other)
                        {
                            if (range.child == node.lod->ranges[other].child)
                                return false;
                        }
                    }
                }
                else if (node.lod)
                    return false;

                if ((node.kind == RenderCore::ModelNodeKind::Billboard) != node.billboard.has_value())
                    return false;
                if ((node.kind == RenderCore::ModelNodeKind::Sort) != node.sort.has_value())
                    return false;
            }

            return true;
        }
    };
}

#endif
