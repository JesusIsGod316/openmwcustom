#ifndef OPENMW_COMPONENTS_RENDERCORE_REALIZATIONKEYS_H
#define OPENMW_COMPONENTS_RENDERCORE_REALIZATIONKEYS_H

#include "records.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace RenderCore
{
    // Revision the stable fingerprints below whenever the key contract changes.
    // Equality still compares the complete key, so this is a persistent-cache
    // compatibility marker rather than a substitute for structural equality.
    inline constexpr std::uint32_t RealizationKeySchemaRevision = 1u;

    struct TextureViewKey
    {
        TextureHandle texture;
        TextureColorSpace colorSpace = TextureColorSpace::Srgb;
        TextureFormatClass formatClass = TextureFormatClass::Unknown;

        [[nodiscard]] bool valid() const noexcept { return texture.valid(); }
        friend bool operator==(const TextureViewKey&, const TextureViewKey&) = default;
    };

    // Sampler realization is intentionally independent of image/view identity.
    // Anisotropy is stored as canonical IEEE bits so equality and hashing never
    // depend on implementation-defined floating-point hashing behavior.
    struct SamplerRealizationKey
    {
        TextureFilter minFilter = TextureFilter::Linear;
        TextureFilter magFilter = TextureFilter::Linear;
        TextureMipmapMode mipmapMode = TextureMipmapMode::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        std::uint32_t maxAnisotropyBits = 0u;

        friend bool operator==(const SamplerRealizationKey&, const SamplerRealizationKey&) = default;
    };

    struct VertexLayoutKey
    {
        bool normals = false;
        bool tangentBasis = false;
        bool colors = false;
        std::uint32_t texCoordSetCount = 0u;

        friend bool operator==(const VertexLayoutKey&, const VertexLayoutKey&) = default;
    };

    // Descriptor/shader layout identity only. The logical image handle, sampler,
    // and numeric transform values are deliberately absent so changing content or
    // sampling state cannot fragment graphics-pipeline caches.
    struct TextureBindingLayoutKey
    {
        TextureRole role = TextureRole::Diffuse;
        TextureColorSpace colorSpace = TextureColorSpace::Srgb;
        TextureFormatClass formatClass = TextureFormatClass::Unknown;
        std::uint32_t uvSet = 0u;

        friend bool operator==(const TextureBindingLayoutKey&, const TextureBindingLayoutKey&) = default;
    };

    struct ShaderFeatureKey
    {
        TextureApplyMode textureApply = TextureApplyMode::Modulate;
        VertexColorMode vertexColorMode = VertexColorMode::Ignore;
        bool unlit = false;
        bool alphaTestEnabled = false;
        CompareOp alphaCompare = CompareOp::Always;
        bool bumpParametersEnabled = false;
        MaterialFogMode fogMode = MaterialFogMode::Inherit;
        bool treeAnimation = false;
        bool refraction = false;
        bool softEffect = false;
        bool falloff = false;
        std::vector<TextureBindingLayoutKey> textures;

        friend bool operator==(const ShaderFeatureKey&, const ShaderFeatureKey&) = default;
    };

    // Pass routing is broader than backend graphics-pipeline identity. Transparent
    // sorting, for example, changes submission strategy but not pipeline state.
    struct StaticPassKey
    {
        bool alphaBlendEnabled = false;
        bool alphaTestEnabled = false;
        bool decal = false;
        TransparentSortPolicy transparentSort = TransparentSortPolicy::Inherit;
        bool refraction = false;
        bool softEffect = false;

        friend bool operator==(const StaticPassKey&, const StaticPassKey&) = default;
    };

    struct BlendStateKey
    {
        bool enabled = false;
        BlendFactor source = BlendFactor::One;
        BlendFactor destination = BlendFactor::Zero;
        BlendEquation equation = BlendEquation::Add;

        friend bool operator==(const BlendStateKey&, const BlendStateKey&) = default;
    };

    struct RasterStateKey
    {
        CullMode cullMode = CullMode::Back;
        FrontFaceWinding frontFace = FrontFaceWinding::CounterClockwise;
        bool wireframe = false;
        // Backend chooses the actual depth-bias convention (including reversed
        // depth signs); the neutral key carries only the semantic decal request.
        bool decal = false;

        friend bool operator==(const RasterStateKey&, const RasterStateKey&) = default;
    };

    struct DepthStencilStateKey
    {
        bool depthTest = true;
        bool depthWrite = true;
        bool stencilEnabled = false;
        CompareOp stencilCompare = CompareOp::Always;
        std::uint32_t stencilReference = 0u;
        std::uint32_t stencilCompareMask = 0u;
        StencilOp stencilFail = StencilOp::Keep;
        StencilOp stencilDepthFail = StencilOp::Keep;
        StencilOp stencilPass = StencilOp::Keep;

        friend bool operator==(const DepthStencilStateKey&, const DepthStencilStateKey&) = default;
    };

    struct FixedFunctionPipelineKey
    {
        BlendStateKey blend;
        RasterStateKey raster;
        DepthStencilStateKey depthStencil;

        friend bool operator==(const FixedFunctionPipelineKey&, const FixedFunctionPipelineKey&) = default;
    };

    // Material realization includes draw/pass routing in addition to the shader
    // and fixed-function state. Numeric uniforms, source/content identity,
    // TextureHandles, samplers, and transform values are intentionally excluded.
    struct MaterialRealizationKey
    {
        ShaderFeatureKey shader;
        StaticPassKey pass;
        FixedFunctionPipelineKey fixedFunction;

        friend bool operator==(const MaterialRealizationKey&, const MaterialRealizationKey&) = default;
    };

    // Canonical backend graphics-pipeline identity. It is derived exclusively
    // from neutral vertex layout, primitive topology and static material state.
    // No NIF type, OSG/VSG object, pointer, VFS path or logical image identity is
    // representable here by construction.
    struct GraphicsPipelineKey
    {
        VertexLayoutKey vertexLayout;
        PrimitiveTopology topology = PrimitiveTopology::Triangles;
        ShaderFeatureKey shader;
        FixedFunctionPipelineKey fixedFunction;

        friend bool operator==(const GraphicsPipelineKey&, const GraphicsPipelineKey&) = default;
    };

    [[nodiscard]] inline TextureViewKey makeTextureViewKey(const TextureBinding& binding) noexcept
    {
        return { binding.texture, binding.colorSpace, binding.formatClass };
    }

    [[nodiscard]] inline std::optional<SamplerRealizationKey> makeSamplerRealizationKey(
        const SamplerSemantic& sampler) noexcept
    {
        if (!std::isfinite(sampler.maxAnisotropy) || sampler.maxAnisotropy < 0.0f)
            return std::nullopt;

        const float canonicalAnisotropy = sampler.maxAnisotropy == 0.0f ? 0.0f : sampler.maxAnisotropy;
        return SamplerRealizationKey{ sampler.minFilter, sampler.magFilter, sampler.mipmapMode, sampler.wrapU,
            sampler.wrapV, std::bit_cast<std::uint32_t>(canonicalAnisotropy) };
    }

    [[nodiscard]] inline VertexLayoutKey makeVertexLayoutKey(const MeshPayload& payload) noexcept
    {
        return VertexLayoutKey{ !payload.normals.empty(), !payload.tangents.empty(), !payload.colors.empty(),
            static_cast<std::uint32_t>(payload.texCoordSets.size()) };
    }

    [[nodiscard]] inline TextureBindingLayoutKey makeTextureBindingLayoutKey(const TextureBinding& binding) noexcept
    {
        return { binding.role, binding.colorSpace, binding.formatClass, binding.transform.uvSet };
    }

    [[nodiscard]] inline std::vector<TextureBindingLayoutKey> makeCanonicalTextureLayout(
        const MaterialRecord& material)
    {
        std::vector<TextureBindingLayoutKey> result;
        result.reserve(material.textures.size());
        for (const TextureBinding& binding : material.textures)
            result.push_back(makeTextureBindingLayoutKey(binding));

        std::sort(result.begin(), result.end(), [](const TextureBindingLayoutKey& lhs, const TextureBindingLayoutKey& rhs) {
            return std::tuple{ static_cast<std::uint8_t>(lhs.role), static_cast<std::uint8_t>(lhs.colorSpace),
                       static_cast<std::uint8_t>(lhs.formatClass), lhs.uvSet }
                < std::tuple{ static_cast<std::uint8_t>(rhs.role), static_cast<std::uint8_t>(rhs.colorSpace),
                    static_cast<std::uint8_t>(rhs.formatClass), rhs.uvSet };
        });
        return result;
    }

    [[nodiscard]] inline ShaderFeatureKey makeShaderFeatureKey(const MaterialRecord& material)
    {
        ShaderFeatureKey result;
        result.textureApply = material.textureApply;
        result.vertexColorMode = material.vertexColorMode;
        result.unlit = material.unlit;
        result.alphaTestEnabled = material.alphaTestEnabled;
        result.alphaCompare = material.alphaTestEnabled ? material.alphaCompare : CompareOp::Always;
        result.bumpParametersEnabled = material.bumpParametersEnabled;
        result.fogMode = material.fog.mode;
        result.treeAnimation = material.treeAnimation;
        result.refraction = material.refraction;
        result.softEffect = material.softEffect;
        result.falloff = material.falloff;
        result.textures = makeCanonicalTextureLayout(material);
        return result;
    }

    [[nodiscard]] inline StaticPassKey makeStaticPassKey(const MaterialRecord& material) noexcept
    {
        return { material.alphaBlendEnabled, material.alphaTestEnabled, material.decal, material.transparentSort,
            material.refraction, material.softEffect };
    }

    [[nodiscard]] inline FixedFunctionPipelineKey makeFixedFunctionPipelineKey(const MaterialRecord& material) noexcept
    {
        FixedFunctionPipelineKey result;
        result.blend.enabled = material.alphaBlendEnabled;
        if (material.alphaBlendEnabled)
        {
            result.blend.source = material.sourceBlend;
            result.blend.destination = material.destinationBlend;
            result.blend.equation = material.blendEquation;
        }

        result.raster.cullMode = material.cullMode;
        result.raster.frontFace = material.frontFace;
        result.raster.wireframe = material.wireframe;
        result.raster.decal = material.decal;

        result.depthStencil.depthTest = material.depthTest;
        result.depthStencil.depthWrite = material.depthWrite;
        result.depthStencil.stencilEnabled = material.stencil.enabled;
        if (material.stencil.enabled)
        {
            result.depthStencil.stencilCompare = material.stencil.compare;
            result.depthStencil.stencilReference = material.stencil.reference;
            result.depthStencil.stencilCompareMask = material.stencil.compareMask;
            result.depthStencil.stencilFail = material.stencil.fail;
            result.depthStencil.stencilDepthFail = material.stencil.depthFail;
            result.depthStencil.stencilPass = material.stencil.pass;
        }
        return result;
    }

    [[nodiscard]] inline MaterialRealizationKey makeMaterialRealizationKey(const MaterialRecord& material)
    {
        return { makeShaderFeatureKey(material), makeStaticPassKey(material), makeFixedFunctionPipelineKey(material) };
    }

    [[nodiscard]] inline GraphicsPipelineKey makeGraphicsPipelineKey(
        const MeshPayload& payload, PrimitiveTopology topology, const MaterialRecord& material)
    {
        return { makeVertexLayoutKey(payload), topology, makeShaderFeatureKey(material),
            makeFixedFunctionPipelineKey(material) };
    }

    namespace realization_key_detail
    {
        inline constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
        inline constexpr std::uint64_t FnvPrime = 1099511628211ull;

        inline void observeByte(std::uint64_t& hash, std::uint8_t value) noexcept
        {
            hash ^= value;
            hash *= FnvPrime;
        }

        template <class Integer>
        inline void observeInteger(std::uint64_t& hash, Integer value) noexcept
        {
            using Unsigned = std::make_unsigned_t<Integer>;
            const Unsigned normalized = static_cast<Unsigned>(value);
            for (unsigned int shift = 0; shift < sizeof(Unsigned) * 8u; shift += 8u)
                observeByte(hash, static_cast<std::uint8_t>((normalized >> shift) & static_cast<Unsigned>(0xffu)));
        }

        template <class Enum>
        inline void observeEnum(std::uint64_t& hash, Enum value) noexcept
        {
            observeInteger(hash, static_cast<std::underlying_type_t<Enum>>(value));
        }

        inline void observeBool(std::uint64_t& hash, bool value) noexcept
        {
            observeByte(hash, value ? 1u : 0u);
        }

        inline void observeTextureLayout(std::uint64_t& hash, const TextureBindingLayoutKey& key) noexcept
        {
            observeEnum(hash, key.role);
            observeEnum(hash, key.colorSpace);
            observeEnum(hash, key.formatClass);
            observeInteger(hash, key.uvSet);
        }

        inline void observeShader(std::uint64_t& hash, const ShaderFeatureKey& key) noexcept
        {
            observeEnum(hash, key.textureApply);
            observeEnum(hash, key.vertexColorMode);
            observeBool(hash, key.unlit);
            observeBool(hash, key.alphaTestEnabled);
            observeEnum(hash, key.alphaCompare);
            observeBool(hash, key.bumpParametersEnabled);
            observeEnum(hash, key.fogMode);
            observeBool(hash, key.treeAnimation);
            observeBool(hash, key.refraction);
            observeBool(hash, key.softEffect);
            observeBool(hash, key.falloff);
            observeInteger(hash, static_cast<std::uint64_t>(key.textures.size()));
            for (const TextureBindingLayoutKey& texture : key.textures)
                observeTextureLayout(hash, texture);
        }

        inline void observeFixedFunction(std::uint64_t& hash, const FixedFunctionPipelineKey& key) noexcept
        {
            observeBool(hash, key.blend.enabled);
            observeEnum(hash, key.blend.source);
            observeEnum(hash, key.blend.destination);
            observeEnum(hash, key.blend.equation);
            observeEnum(hash, key.raster.cullMode);
            observeEnum(hash, key.raster.frontFace);
            observeBool(hash, key.raster.wireframe);
            observeBool(hash, key.raster.decal);
            observeBool(hash, key.depthStencil.depthTest);
            observeBool(hash, key.depthStencil.depthWrite);
            observeBool(hash, key.depthStencil.stencilEnabled);
            observeEnum(hash, key.depthStencil.stencilCompare);
            observeInteger(hash, key.depthStencil.stencilReference);
            observeInteger(hash, key.depthStencil.stencilCompareMask);
            observeEnum(hash, key.depthStencil.stencilFail);
            observeEnum(hash, key.depthStencil.stencilDepthFail);
            observeEnum(hash, key.depthStencil.stencilPass);
        }

        inline void observePass(std::uint64_t& hash, const StaticPassKey& key) noexcept
        {
            observeBool(hash, key.alphaBlendEnabled);
            observeBool(hash, key.alphaTestEnabled);
            observeBool(hash, key.decal);
            observeEnum(hash, key.transparentSort);
            observeBool(hash, key.refraction);
            observeBool(hash, key.softEffect);
        }
    }

    [[nodiscard]] inline std::uint64_t stableTextureViewFingerprint(const TextureViewKey& key) noexcept
    {
        std::uint64_t hash = realization_key_detail::FnvOffset;
        realization_key_detail::observeInteger(hash, RealizationKeySchemaRevision);
        realization_key_detail::observeInteger(hash, key.texture.slot());
        realization_key_detail::observeInteger(hash, key.texture.generation());
        realization_key_detail::observeEnum(hash, key.colorSpace);
        realization_key_detail::observeEnum(hash, key.formatClass);
        return hash;
    }

    [[nodiscard]] inline std::uint64_t stableSamplerFingerprint(const SamplerRealizationKey& key) noexcept
    {
        std::uint64_t hash = realization_key_detail::FnvOffset;
        realization_key_detail::observeInteger(hash, RealizationKeySchemaRevision);
        realization_key_detail::observeEnum(hash, key.minFilter);
        realization_key_detail::observeEnum(hash, key.magFilter);
        realization_key_detail::observeEnum(hash, key.mipmapMode);
        realization_key_detail::observeEnum(hash, key.wrapU);
        realization_key_detail::observeEnum(hash, key.wrapV);
        realization_key_detail::observeInteger(hash, key.maxAnisotropyBits);
        return hash;
    }

    [[nodiscard]] inline std::uint64_t stableMaterialRealizationFingerprint(const MaterialRealizationKey& key) noexcept
    {
        std::uint64_t hash = realization_key_detail::FnvOffset;
        realization_key_detail::observeInteger(hash, RealizationKeySchemaRevision);
        realization_key_detail::observeShader(hash, key.shader);
        realization_key_detail::observePass(hash, key.pass);
        realization_key_detail::observeFixedFunction(hash, key.fixedFunction);
        return hash;
    }

    [[nodiscard]] inline std::uint64_t stableGraphicsPipelineFingerprint(const GraphicsPipelineKey& key) noexcept
    {
        std::uint64_t hash = realization_key_detail::FnvOffset;
        realization_key_detail::observeInteger(hash, RealizationKeySchemaRevision);
        realization_key_detail::observeBool(hash, key.vertexLayout.normals);
        realization_key_detail::observeBool(hash, key.vertexLayout.tangentBasis);
        realization_key_detail::observeBool(hash, key.vertexLayout.colors);
        realization_key_detail::observeInteger(hash, key.vertexLayout.texCoordSetCount);
        realization_key_detail::observeEnum(hash, key.topology);
        realization_key_detail::observeShader(hash, key.shader);
        realization_key_detail::observeFixedFunction(hash, key.fixedFunction);
        return hash;
    }

    struct TextureViewKeyHash
    {
        [[nodiscard]] std::size_t operator()(const TextureViewKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableTextureViewFingerprint(key));
        }
    };

    struct SamplerRealizationKeyHash
    {
        [[nodiscard]] std::size_t operator()(const SamplerRealizationKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableSamplerFingerprint(key));
        }
    };

    struct MaterialRealizationKeyHash
    {
        [[nodiscard]] std::size_t operator()(const MaterialRealizationKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableMaterialRealizationFingerprint(key));
        }
    };

    struct GraphicsPipelineKeyHash
    {
        [[nodiscard]] std::size_t operator()(const GraphicsPipelineKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableGraphicsPipelineFingerprint(key));
        }
    };
}

#endif
