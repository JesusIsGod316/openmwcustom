#ifndef OPENMW_COMPONENTS_RENDERCORE_RECORDS_H
#define OPENMW_COMPONENTS_RENDERCORE_RECORDS_H

#include "handles.hpp"
#include "math.hpp"
#include "resources.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RenderCore
{
    namespace semantic_detail
    {
        [[nodiscard]] inline bool finite(float value) noexcept
        {
            return std::isfinite(value);
        }

        [[nodiscard]] inline bool finite(double value) noexcept
        {
            return std::isfinite(value);
        }

        [[nodiscard]] inline bool finite(const glm::vec2& value) noexcept
        {
            return finite(value.x) && finite(value.y);
        }

        [[nodiscard]] inline bool finite(const glm::vec3& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] inline bool finite(const glm::dvec3& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] inline bool finite(const glm::vec4& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
        }

        [[nodiscard]] inline bool finite(const glm::quat& value) noexcept
        {
            return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
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

        [[nodiscard]] inline bool finite(const LocalTransform& value) noexcept
        {
            return finite(value.translation) && finite(value.rotation) && finite(value.scale);
        }
    }

    enum class PrimitiveTopology : std::uint8_t
    {
        Triangles,
        TriangleStrip,
        Lines,
        Points,
    };

    enum class TextureRole : std::uint8_t
    {
        Diffuse,
        Dark,
        Detail,
        Decal,
        Emissive,
        Normal,
        Environment,
        Specular,
        Bump,
        Gloss,
        Blend,
    };

    enum class TextureColorSpace : std::uint8_t
    {
        Linear,
        Srgb,
        Data,
    };

    enum class TextureFormatClass : std::uint8_t
    {
        Unknown,
        Color,
        Normal,
        Scalar,
        Height,
    };

    enum class TextureFilter : std::uint8_t
    {
        Nearest,
        Linear,
    };

    enum class TextureMipmapMode : std::uint8_t
    {
        None,
        Nearest,
        Linear,
    };

    enum class TextureWrap : std::uint8_t
    {
        Repeat,
        Clamp,
        Mirror,
        Border,
    };

    enum class TextureApplyMode : std::uint8_t
    {
        Replace,
        Decal,
        Modulate,
        Highlight,
        Highlight2,
    };

    // The source translator preserves the authored transform convention so a
    // backend can reproduce OpenMW's exact matrix ordering without retaining a
    // NIF type. Direct is the canonical center/scale/rotate/offset convention
    // used by backend-authored and already-normalized transforms.
    enum class TextureTransformConvention : std::uint8_t
    {
        Direct,
        MayaLegacy,
        Max,
        Maya,
    };

    enum class AlphaMode : std::uint8_t
    {
        Opaque,
        Mask,
        Blend,
    };

    enum class CompareOp : std::uint8_t
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    enum class BlendFactor : std::uint8_t
    {
        One,
        Zero,
        SourceColor,
        OneMinusSourceColor,
        DestinationColor,
        OneMinusDestinationColor,
        SourceAlpha,
        OneMinusSourceAlpha,
        DestinationAlpha,
        OneMinusDestinationAlpha,
        SourceAlphaSaturate,
    };

    enum class BlendEquation : std::uint8_t
    {
        Add,
        Subtract,
        ReverseSubtract,
        Minimum,
        Maximum,
    };

    enum class CullMode : std::uint8_t
    {
        None,
        Back,
        Front,
    };

    enum class FrontFaceWinding : std::uint8_t
    {
        CounterClockwise,
        Clockwise,
    };

    enum class TransparentSortPolicy : std::uint8_t
    {
        Inherit,
        Sorted,
        Unsorted,
    };

    enum class StencilOp : std::uint8_t
    {
        Keep,
        Zero,
        Replace,
        Increment,
        Decrement,
        Invert,
    };

    enum class VertexColorMode : std::uint8_t
    {
        Ignore,
        Emissive,
        AmbientDiffuse,
    };

    struct TextureTransform
    {
        glm::vec2 offset{ 0.0f, 0.0f };
        glm::vec2 scale{ 1.0f, 1.0f };
        glm::vec2 center{ 0.5f, 0.5f };
        float rotation = 0.0f;
        std::uint32_t uvSet = 0;
        TextureTransformConvention convention = TextureTransformConvention::Direct;
    };

    struct SamplerSemantic
    {
        TextureFilter minFilter = TextureFilter::Linear;
        TextureFilter magFilter = TextureFilter::Linear;
        TextureMipmapMode mipmapMode = TextureMipmapMode::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        float maxAnisotropy = 0.0f;
    };

    struct TextureBinding
    {
        TextureRole role = TextureRole::Diffuse;
        TextureHandle texture;
        // Decode/interpretation belongs to the material use, not the image identity.
        // One resident image may legitimately be sampled as color or data by
        // different OpenMW material semantics without duplicating its logical asset.
        TextureColorSpace colorSpace = TextureColorSpace::Srgb;
        TextureFormatClass formatClass = TextureFormatClass::Unknown;
        TextureTransform transform;
        SamplerSemantic sampler;
    };

    struct MeshSurface
    {
        PrimitiveTopology topology = PrimitiveTopology::Triangles;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t materialSlot = 0;
    };

    struct MeshPayload
    {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        // Tangent and bitangent are kept as separate neutral streams because
        // both legacy NiGeometryData and packed BS geometry preserve the full
        // basis. Vulkan realization may repack this into tangent+handedness.
        std::vector<glm::vec3> tangents;
        std::vector<glm::vec3> bitangents;
        std::vector<glm::vec4> colors;
        std::vector<std::vector<glm::vec2>> texCoordSets;
        std::vector<std::uint32_t> indices;
        std::vector<MeshSurface> surfaces;
    };

    // Skin indices are local to SkinPayload::bones. Keeping the source skin's
    // compact bone palette on the mesh avoids assuming that every body part or
    // equipment mesh uses the same full actor-skeleton indices. Realization
    // resolves the canonical bone names against the instance SkeletonRecord.
    struct SkinInfluence
    {
        std::uint32_t boneIndex = 0;
        float weight = 0.0f;
    };

    struct SkinBoneBinding
    {
        std::string name;
        glm::mat4 inverseBind{ 1.0f };
    };

    struct SkinPayload
    {
        glm::mat4 meshToSkeleton{ 1.0f };
        std::string rootBoneName;
        std::vector<SkinBoneBinding> bones;
        // Exactly one influence list per source vertex. Empty lists are kept:
        // the OpenGL compatibility path leaves those vertices undeformed.
        std::vector<std::vector<SkinInfluence>> vertexInfluences;
    };

    struct MorphTargetPayload
    {
        std::string name;
        // Original controller target index. Legacy NiGeomMorpherController
        // reserves source target zero as the absolute base shape, so published
        // deforming targets normally begin at one.
        std::uint32_t sourceIndex = 0;
        // Canonical offsets applied to MeshPayload::positions. The NIF adapter
        // preserves OpenMW's established interpretation of targets one..N as
        // offsets even when source metadata describes another convention.
        std::vector<glm::vec3> positionOffsets;
    };

    struct MorphPayload
    {
        std::vector<MorphTargetPayload> targets;
    };

    [[nodiscard]] inline bool validSkinPayload(const SkinPayload& skin, std::size_t vertexCount) noexcept
    {
        if (!semantic_detail::finite(skin.meshToSkeleton) || skin.bones.empty()
            || skin.vertexInfluences.size() != vertexCount)
            return false;
        for (std::size_t i = 0; i < skin.bones.size(); ++i)
        {
            const SkinBoneBinding& bone = skin.bones[i];
            if (bone.name.empty() || !semantic_detail::finite(bone.inverseBind))
                return false;
            for (std::size_t other = i + 1; other < skin.bones.size(); ++other)
            {
                if (bone.name == skin.bones[other].name)
                    return false;
            }
        }
        for (const auto& influences : skin.vertexInfluences)
        {
            for (std::size_t i = 0; i < influences.size(); ++i)
            {
                const SkinInfluence& influence = influences[i];
                if (influence.boneIndex >= skin.bones.size() || !std::isfinite(influence.weight)
                    || influence.weight < 0.0f)
                    return false;
                for (std::size_t other = i + 1; other < influences.size(); ++other)
                {
                    if (influence.boneIndex == influences[other].boneIndex)
                        return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] inline bool validMorphPayload(const MorphPayload& morphs, std::size_t vertexCount) noexcept
    {
        if (morphs.targets.empty())
            return false;
        for (std::size_t i = 0; i < morphs.targets.size(); ++i)
        {
            const MorphTargetPayload& target = morphs.targets[i];
            if (target.sourceIndex == 0 || target.positionOffsets.size() != vertexCount)
                return false;
            for (std::size_t other = i + 1; other < morphs.targets.size(); ++other)
            {
                if (target.sourceIndex == morphs.targets[other].sourceIndex)
                    return false;
            }
            for (const glm::vec3& offset : target.positionOffsets)
            {
                if (!semantic_detail::finite(offset))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool validMeshPayload(const MeshPayload& payload) noexcept
    {
        const std::size_t vertexCount = payload.positions.size();
        if (!payload.normals.empty() && payload.normals.size() != vertexCount)
            return false;
        if (payload.tangents.empty() != payload.bitangents.empty())
            return false;
        if (!payload.tangents.empty()
            && (payload.tangents.size() != vertexCount || payload.bitangents.size() != vertexCount))
            return false;
        if (!payload.colors.empty() && payload.colors.size() != vertexCount)
            return false;
        for (const auto& texCoords : payload.texCoordSets)
        {
            if (!texCoords.empty() && texCoords.size() != vertexCount)
                return false;
        }

        for (const glm::vec3& position : payload.positions)
        {
            if (!semantic_detail::finite(position))
                return false;
        }
        for (const glm::vec3& normal : payload.normals)
        {
            if (!semantic_detail::finite(normal))
                return false;
        }
        for (const glm::vec3& tangent : payload.tangents)
        {
            if (!semantic_detail::finite(tangent))
                return false;
        }
        for (const glm::vec3& bitangent : payload.bitangents)
        {
            if (!semantic_detail::finite(bitangent))
                return false;
        }
        for (const glm::vec4& color : payload.colors)
        {
            if (!semantic_detail::finite(color))
                return false;
        }
        for (const auto& texCoords : payload.texCoordSets)
        {
            for (const glm::vec2& texCoord : texCoords)
            {
                if (!semantic_detail::finite(texCoord))
                    return false;
            }
        }
        for (const std::uint32_t index : payload.indices)
        {
            if (index >= vertexCount)
                return false;
        }

        for (const MeshSurface& surface : payload.surfaces)
        {
            if (surface.indexCount == 0 || surface.firstIndex > payload.indices.size()
                || surface.indexCount > payload.indices.size() - surface.firstIndex)
                return false;
        }
        return true;
    }

    struct MeshRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
        AxisAlignedBounds bounds;
        std::uint32_t surfaceCount = 0;
        bool skinned = false;
        bool morphed = false;
        std::shared_ptr<const MeshPayload> payload;
        std::shared_ptr<const SkinPayload> skin;
        std::shared_ptr<const MorphPayload> morphs;
    };

    struct StencilSemantic
    {
        bool enabled = false;
        CompareOp compare = CompareOp::Always;
        std::uint32_t reference = 0;
        std::uint32_t compareMask = std::numeric_limits<std::uint32_t>::max();
        StencilOp fail = StencilOp::Keep;
        StencilOp depthFail = StencilOp::Keep;
        StencilOp pass = StencilOp::Keep;
    };

    enum class MaterialFogMode : std::uint8_t
    {
        Inherit,
        Disabled,
        Override,
    };

    struct MaterialFogSemantic
    {
        MaterialFogMode mode = MaterialFogMode::Inherit;
        Color color{ 0.0f, 0.0f, 0.0f, 1.0f };
        float depth = 0.0f;
    };

    struct MaterialRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
        Color diffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color ambient{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color specular{ 0.0f, 0.0f, 0.0f, 1.0f };
        Color emission{ 0.0f, 0.0f, 0.0f, 1.0f };
        Color environmentMapColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        float shininess = 0.0f;
        float emissiveMultiplier = 1.0f;
        float specularStrength = 1.0f;
        float environmentMapStrength = 0.0f;
        float alpha = 1.0f;
        float alphaCutoff = 0.5f;
        // alphaMode is a coarse pass-class hint. The booleans and compare op
        // below are authoritative because legacy content can enable blending
        // and testing simultaneously.
        AlphaMode alphaMode = AlphaMode::Opaque;
        bool alphaBlendEnabled = false;
        bool alphaTestEnabled = false;
        CompareOp alphaCompare = CompareOp::Always;
        TransparentSortPolicy transparentSort = TransparentSortPolicy::Inherit;
        BlendFactor sourceBlend = BlendFactor::SourceAlpha;
        BlendFactor destinationBlend = BlendFactor::OneMinusSourceAlpha;
        BlendEquation blendEquation = BlendEquation::Add;
        TextureApplyMode textureApply = TextureApplyMode::Modulate;
        CullMode cullMode = CullMode::Back;
        FrontFaceWinding frontFace = FrontFaceWinding::CounterClockwise;
        StencilSemantic stencil;
        VertexColorMode vertexColorMode = VertexColorMode::Ignore;
        bool wireframe = false;
        bool unlit = false;
        bool depthTest = true;
        bool depthWrite = true;
        // These are realized V3.25 rendering semantics, not raw NIF/BGSM schema
        // fields. Backend-specific implementation details (OSG state objects,
        // reversed-depth polygon-offset signs, effect helper nodes) deliberately
        // remain outside the neutral contract.
        bool decal = false;
        bool bumpParametersEnabled = false;
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
        std::vector<TextureBinding> textures;
    };

    struct TextureRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        // Source identity records normalized producer/VFS provenance. Content
        // identity names the logical image bytes and is the canonical dedup key.
        // Binding interpretation (sRGB/linear/data) and sampler state are never
        // part of either identity; backend realization may key those variants
        // separately from this logical TextureHandle.
        std::string sourceIdentity;
        std::string contentIdentity;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool mipmapped = true;
    };

    inline constexpr std::uint32_t InvalidModelNodeIndex = std::numeric_limits<std::uint32_t>::max();

    class ModelNodeIndex final
    {
    public:
        constexpr ModelNodeIndex() noexcept = default;
        explicit constexpr ModelNodeIndex(std::uint32_t value) noexcept
            : mValue(value)
        {
        }

        [[nodiscard]] constexpr bool valid() const noexcept { return mValue != InvalidModelNodeIndex; }
        explicit constexpr operator bool() const noexcept { return valid(); }
        [[nodiscard]] constexpr std::uint32_t value() const noexcept { return mValue; }

        friend constexpr bool operator==(ModelNodeIndex, ModelNodeIndex) noexcept = default;

    private:
        std::uint32_t mValue = InvalidModelNodeIndex;
    };

    enum class ModelNodeKind : std::uint8_t
    {
        Transform,
        Geometry,
        Switch,
        Lod,
        Billboard,
        Sort,
    };

    enum class ModelBillboardMode : std::uint8_t
    {
        AlwaysFaceCamera,
        RotateAboutUp,
        RigidFaceCamera,
        AlwaysFaceCenter,
        RigidFaceCenter,
        RotateAboutUpBethesda,
    };

    enum class ModelSortMode : std::uint8_t
    {
        Inherit,
        Off,
        Subsort,
    };

    enum class ModelSortAccumulator : std::uint8_t
    {
        Missing,
        Alpha,
        Cluster,
        Unsupported,
    };

    struct ModelSortSemantic
    {
        ModelSortMode mode = ModelSortMode::Inherit;
        ModelSortAccumulator accumulator = ModelSortAccumulator::Missing;
    };

    enum class ModelNodeFlag : std::uint32_t
    {
        Hidden = 1u << 0,
        Collision = 1u << 1,
        CollisionOnly = 1u << 2,
        Marker = 1u << 3,
        ControllerTarget = 1u << 4,
    };

    [[nodiscard]] constexpr std::uint32_t modelNodeFlag(ModelNodeFlag flag) noexcept
    {
        return static_cast<std::uint32_t>(flag);
    }

    enum class ModelControllerFlag : std::uint32_t
    {
        Transform = 1u << 0,
        Morph = 1u << 1,
        Visibility = 1u << 2,
        Unsupported = 1u << 3,
    };

    [[nodiscard]] constexpr std::uint32_t modelControllerFlag(ModelControllerFlag flag) noexcept
    {
        return static_cast<std::uint32_t>(flag);
    }

    [[nodiscard]] constexpr bool validModelControllerFlags(std::uint32_t flags) noexcept
    {
        constexpr std::uint32_t all = modelControllerFlag(ModelControllerFlag::Transform)
            | modelControllerFlag(ModelControllerFlag::Morph) | modelControllerFlag(ModelControllerFlag::Visibility)
            | modelControllerFlag(ModelControllerFlag::Unsupported);
        return (flags & ~all) == 0;
    }

    enum class ModelDynamicRequirement : std::uint32_t
    {
        ParticleSystem = 1u << 0,
        NodeEffect = 1u << 1,
        SequencePlayback = 1u << 2,
        DynamicVertexData = 1u << 3,
    };

    [[nodiscard]] constexpr std::uint32_t modelDynamicRequirement(ModelDynamicRequirement value) noexcept
    {
        return static_cast<std::uint32_t>(value);
    }

    [[nodiscard]] constexpr bool validModelDynamicRequirements(std::uint32_t requirements) noexcept
    {
        constexpr std::uint32_t all = modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem)
            | modelDynamicRequirement(ModelDynamicRequirement::NodeEffect)
            | modelDynamicRequirement(ModelDynamicRequirement::SequencePlayback)
            | modelDynamicRequirement(ModelDynamicRequirement::DynamicVertexData);
        return (requirements & ~all) == 0;
    }

    struct ModelLodRange
    {
        ModelNodeIndex child;
        float minimumDistance = 0.0f;
        float maximumDistance = std::numeric_limits<float>::max();
    };

    struct ModelLodSemantic
    {
        glm::vec3 center{ 0.0f, 0.0f, 0.0f };
        std::vector<ModelLodRange> ranges;
    };

    struct ModelNodeRecord
    {
        std::string name;
        // Diagnostic/source-local provenance only. This is never a backend
        // resource or pipeline identity.
        std::optional<std::uint32_t> sourceRecordId;
        ModelNodeIndex parent;
        // Preserve the exact parsed local affine transform. NIF rotation matrices
        // may legally contain reflected/non-uniform scale components that are not
        // safely representable as quaternion + scale decomposition.
        glm::mat4 localTransform{ 1.0f };
        ModelNodeKind kind = ModelNodeKind::Transform;
        std::optional<MeshHandle> mesh;
        std::vector<MaterialHandle> materials;
        std::optional<ModelNodeIndex> activeSwitchChild;
        std::optional<ModelLodSemantic> lod;
        std::optional<ModelBillboardMode> billboard;
        std::optional<ModelSortSemantic> sort;
        std::uint32_t flags = 0;
        std::uint32_t controllerFlags = 0;
    };

    struct ModelPayload
    {
        // Nodes are immutable and topologically ordered: every parent precedes
        // its children. Local indices are therefore stable, compact, cacheable,
        // and suitable for deterministic CP3C/CP4 publication and later CP7
        // backend packing without turning RenderCore into a scene graph.
        std::vector<ModelNodeRecord> nodes;
        std::vector<ModelNodeIndex> roots;
    };

    [[nodiscard]] inline bool validModelPayloadStructure(const ModelPayload& payload) noexcept
    {
        std::vector<ModelNodeIndex> expectedRoots;
        std::vector<std::uint32_t> childCounts(payload.nodes.size(), 0);

        for (std::size_t i = 0; i < payload.nodes.size(); ++i)
        {
            const ModelNodeRecord& node = payload.nodes[i];
            if (!semantic_detail::finite(node.localTransform))
                return false;

            if (node.parent.valid())
            {
                if (node.parent.value() >= i)
                    return false;
                ++childCounts[node.parent.value()];
            }
            else
                expectedRoots.emplace_back(static_cast<std::uint32_t>(i));

            if (node.mesh && !node.mesh->valid())
                return false;
            for (const MaterialHandle material : node.materials)
            {
                if (!material.valid())
                    return false;
            }

            if ((node.flags & modelNodeFlag(ModelNodeFlag::CollisionOnly)) != 0
                && (node.flags & modelNodeFlag(ModelNodeFlag::Collision)) == 0)
                return false;
        }

        if (payload.roots != expectedRoots)
            return false;

        for (std::size_t i = 0; i < payload.nodes.size(); ++i)
        {
            const ModelNodeRecord& node = payload.nodes[i];
            if (!validModelControllerFlags(node.controllerFlags)
                || (node.controllerFlags != 0) != ((node.flags & modelNodeFlag(ModelNodeFlag::ControllerTarget)) != 0))
                return false;
            const auto isDirectChild = [&](ModelNodeIndex child) {
                return child.valid() && child.value() < payload.nodes.size()
                    && payload.nodes[child.value()].parent == ModelNodeIndex{ static_cast<std::uint32_t>(i) };
            };

            if (node.kind == ModelNodeKind::Geometry)
            {
                if (!node.mesh)
                    return false;
            }
            else if (node.mesh)
                return false;

            if (node.kind == ModelNodeKind::Switch)
            {
                if (node.activeSwitchChild && !isDirectChild(*node.activeSwitchChild))
                    return false;
            }
            else if (node.activeSwitchChild)
                return false;

            if (node.kind == ModelNodeKind::Lod)
            {
                if (!node.lod || !semantic_detail::finite(node.lod->center)
                    || node.lod->ranges.size() != childCounts[i])
                    return false;

                for (std::size_t rangeIndex = 0; rangeIndex < node.lod->ranges.size(); ++rangeIndex)
                {
                    const ModelLodRange& range = node.lod->ranges[rangeIndex];
                    if (!isDirectChild(range.child) || !semantic_detail::finite(range.minimumDistance)
                        || !semantic_detail::finite(range.maximumDistance)
                        || range.minimumDistance > range.maximumDistance)
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

            if ((node.kind == ModelNodeKind::Billboard) != node.billboard.has_value())
                return false;
            if ((node.kind == ModelNodeKind::Sort) != node.sort.has_value())
                return false;
        }

        return true;
    }

    struct ModelRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        // Source identity is normalized producer/VFS provenance. Content identity
        // is canonical source content hashing; neither is permitted to substitute
        // for backend semantic realization keys.
        std::string sourceIdentity;
        std::string contentIdentity;
        AxisAlignedBounds bounds;
        // Source features which require frame-driven realization beyond the
        // immutable node/material graph. A backend must consume or explicitly
        // reject every bit; it may never infer compatibility from missing nodes.
        std::uint32_t dynamicRequirements = 0;
        std::shared_ptr<const ModelPayload> payload;
    };

    struct BoneRecord
    {
        std::string name;
        std::int32_t parent = -1;
        // Exact local affine bind transform. Actor bones can inherit reflected
        // or non-uniform transforms that are not losslessly representable as a
        // translation/quaternion/uniform-scale tuple.
        glm::mat4 bindLocal{ 1.0f };
        glm::mat4 inverseBind{ 1.0f };
    };

    struct SkeletonPayload
    {
        std::vector<BoneRecord> bones;
    };

    [[nodiscard]] inline bool validSkeletonPayload(const SkeletonPayload& payload) noexcept
    {
        if (payload.bones.empty())
            return false;
        for (std::size_t i = 0; i < payload.bones.size(); ++i)
        {
            const BoneRecord& bone = payload.bones[i];
            const std::int32_t parent = bone.parent;
            if (bone.name.empty() || parent < -1 || (parent >= 0 && static_cast<std::size_t>(parent) >= i))
                return false;
            if (!semantic_detail::finite(bone.bindLocal) || !semantic_detail::finite(bone.inverseBind))
                return false;
            for (std::size_t other = i + 1; other < payload.bones.size(); ++other)
            {
                if (bone.name == payload.bones[other].name)
                    return false;
            }
        }
        return true;
    }

    struct SkeletonRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
        std::shared_ptr<const SkeletonPayload> payload;
    };

    enum class InstanceSemanticFlag : std::uint64_t
    {
        OrdinaryWorld = 1ull << 0,
        OwnerBody = 1ull << 1,
        OwnerHead = 1ull << 2,
        ShadowCaster = 1ull << 3,
        ReflectionEligible = 1ull << 4,
        RefractionEligible = 1ull << 5,
        MapEligible = 1ull << 6,
        PreviewEligible = 1ull << 7,
        Effect = 1ull << 8,
        Projectile = 1ull << 9,
        Groundcover = 1ull << 10,
        Terrain = 1ull << 11,
    };

    [[nodiscard]] constexpr std::uint64_t semanticFlag(InstanceSemanticFlag flag) noexcept
    {
        return static_cast<std::uint64_t>(flag);
    }

    struct LodSemantic
    {
        glm::vec3 center{ 0.0f, 0.0f, 0.0f };
        float minimumDistance = 0.0f;
        float maximumDistance = std::numeric_limits<float>::max();
        float scale = 1.0f;
        bool smallFeatureEligible = true;
    };

    struct AttachmentBinding
    {
        InstanceHandle parent;
        std::optional<SkeletonHandle> skeleton;
        std::optional<std::uint32_t> boneIndex;
        LocalTransform localTransform;
    };

    struct InstanceRecord
    {
        // Instance placement and visibility are mutable independently from the
        // referenced model resources. Backends use this revision together with
        // the generation-safe handle to reject stale placement/residency work
        // without rebuilding every instance after an unrelated world update.
        ResourceRevision revision = InitialResourceRevision;
        // Canonical semantic ownership. ChunkRecord::members is a derived ordered
        // reverse index maintained only by RenderWorld publication operations.
        std::optional<ChunkHandle> chunk;
        // Exactly one asset path is authored. mesh is the compact path for a
        // simple drawable; model preserves compound local hierarchy semantics.
        MeshHandle mesh;
        std::optional<ModelHandle> model;
        std::vector<MaterialHandle> materials;
        std::optional<SkeletonHandle> skeleton;
        std::optional<AttachmentBinding> attachment;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        LodSemantic lod;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster) | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    struct ChunkRecord
    {
        enum class Kind : std::uint8_t
        {
            SceneCell,
            Terrain,
            StaticPopulation,
            Groundcover,
        };

        ResourceRevision revision = InitialResourceRevision;
        std::string producerIdentity;
        std::string worldspaceIdentity;
        AxisAlignedBounds bounds;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld);
        Kind kind = Kind::SceneCell;
        // Stable semantic address for data-oriented exterior populations. The
        // backend may replace its realization and visibility algorithms without
        // changing gameplay ownership or treating a VSG node as chunk identity.
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
        std::uint32_t lodLevel = 0;
        std::uint8_t stitchMask = 0;
        // Derived ordered index for individually addressable instances. Producers
        // never author this independently from InstanceRecord::chunk.
        std::vector<InstanceHandle> members;
    };

    enum class LightModulation : std::uint8_t
    {
        Constant,
        Flicker,
        FlickerSlow,
        Pulse,
        PulseSlow,
    };

    enum class LightSemanticFlag : std::uint8_t
    {
        Dynamic,
        Carryable,
        Negative,
        OffDefault,
        Spot,
        SpotShadow,
    };

    [[nodiscard]] constexpr std::uint64_t lightSemanticFlag(LightSemanticFlag value) noexcept
    {
        return std::uint64_t{ 1 } << static_cast<std::uint8_t>(value);
    }

    struct LightRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        WorldPosition position{ 0.0, 0.0, 0.0 };
        Color diffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color specular{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color ambient{ 0.0f, 0.0f, 0.0f, 1.0f };
        float constantAttenuation = 1.0f;
        float linearAttenuation = 0.0f;
        float quadraticAttenuation = 0.0f;
        float effectiveRadius = 0.0f;
        float actorFade = 1.0f;
        LightModulation modulation = LightModulation::Constant;
        std::uint64_t semanticFlags = 0;
        bool enabled = true;
    };

    struct DynamicTextureTransformState
    {
        std::uint32_t bindingIndex = 0;
        TextureTransform transform;
    };

    struct DynamicMaterialState
    {
        MaterialHandle material;
        std::optional<Color> diffuse;
        std::optional<Color> ambient;
        std::optional<Color> specular;
        std::optional<Color> emission;
        std::optional<float> alpha;
        std::optional<float> emissiveMultiplier;
        std::vector<DynamicTextureTransformState> textureTransforms;
    };
}

#endif
