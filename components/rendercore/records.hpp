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

    enum class AlphaMode : std::uint8_t
    {
        Opaque,
        Mask,
        Blend,
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
        std::vector<glm::vec4> colors;
        std::vector<std::vector<glm::vec2>> texCoordSets;
        std::vector<std::uint32_t> indices;
        std::vector<MeshSurface> surfaces;
    };

    [[nodiscard]] inline bool validMeshPayload(const MeshPayload& payload) noexcept
    {
        const std::size_t vertexCount = payload.positions.size();
        if (!payload.normals.empty() && payload.normals.size() != vertexCount)
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
        AlphaMode alphaMode = AlphaMode::Opaque;
        BlendFactor sourceBlend = BlendFactor::SourceAlpha;
        BlendFactor destinationBlend = BlendFactor::OneMinusSourceAlpha;
        BlendEquation blendEquation = BlendEquation::Add;
        CullMode cullMode = CullMode::Back;
        VertexColorMode vertexColorMode = VertexColorMode::Ignore;
        bool unlit = false;
        bool depthTest = true;
        bool depthWrite = true;
        std::vector<TextureBinding> textures;
    };

    struct TextureRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string sourceIdentity;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool mipmapped = true;
    };

    struct BoneRecord
    {
        std::string name;
        std::int32_t parent = -1;
        LocalTransform bindLocal;
        glm::mat4 inverseBind{ 1.0f };
    };

    struct SkeletonPayload
    {
        std::vector<BoneRecord> bones;
    };

    [[nodiscard]] inline bool validSkeletonPayload(const SkeletonPayload& payload) noexcept
    {
        for (std::size_t i = 0; i < payload.bones.size(); ++i)
        {
            const BoneRecord& bone = payload.bones[i];
            const std::int32_t parent = bone.parent;
            if (parent < -1 || (parent >= 0 && static_cast<std::size_t>(parent) >= i))
                return false;
            if (!semantic_detail::finite(bone.bindLocal) || !semantic_detail::finite(bone.inverseBind))
                return false;
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
        // Canonical semantic ownership. ChunkRecord::members is a derived ordered
        // reverse index maintained only by RenderWorld publication operations.
        std::optional<ChunkHandle> chunk;
        MeshHandle mesh;
        std::vector<MaterialHandle> materials;
        std::optional<SkeletonHandle> skeleton;
        std::optional<AttachmentBinding> attachment;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        LodSemantic lod;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld)
            | semanticFlag(InstanceSemanticFlag::ShadowCaster)
            | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
        bool lightingEnabled = true;
    };

    struct ChunkRecord
    {
        ResourceRevision revision = InitialResourceRevision;
        std::string producerIdentity;
        std::string worldspaceIdentity;
        AxisAlignedBounds bounds;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::OrdinaryWorld);
        // Derived ordered index for individually addressable instances. Producers
        // never author this independently from InstanceRecord::chunk.
        std::vector<InstanceHandle> members;
    };

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
        std::uint64_t semanticFlags = ~std::uint64_t{ 0 };
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
