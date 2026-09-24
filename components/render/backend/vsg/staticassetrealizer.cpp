#include "enchantedmaterialshader.hpp"
#include "legacybumpmaterialshader.hpp"
#include "legacymaterialshader.hpp"
#include "staticassetrealizer.hpp"
#include "livetextureimages.hpp"
#include "persistentpipelinecache.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

#include <vsg/all.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include "viewpipelinebinding.hpp"
#include <vsg/utils/ShaderSet.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] VkPrimitiveTopology toVkTopology(RenderCore::PrimitiveTopology topology) noexcept
        {
            switch (topology)
            {
                case RenderCore::PrimitiveTopology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                case RenderCore::PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                case RenderCore::PrimitiveTopology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                case RenderCore::PrimitiveTopology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            }
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }

        [[nodiscard]] VkCullModeFlags toVkCull(RenderCore::CullMode mode) noexcept
        {
            switch (mode)
            {
                case RenderCore::CullMode::None: return VK_CULL_MODE_NONE;
                case RenderCore::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
                case RenderCore::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            }
            return VK_CULL_MODE_BACK_BIT;
        }

        [[nodiscard]] VkFrontFace toVkFrontFace(RenderCore::FrontFaceWinding winding) noexcept
        {
            return winding == RenderCore::FrontFaceWinding::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }

        [[nodiscard]] VkCompareOp toVkCompare(RenderCore::CompareOp op) noexcept
        {
            switch (op)
            {
                case RenderCore::CompareOp::Never: return VK_COMPARE_OP_NEVER;
                case RenderCore::CompareOp::Less: return VK_COMPARE_OP_LESS;
                case RenderCore::CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
                case RenderCore::CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
                case RenderCore::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
                case RenderCore::CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
                case RenderCore::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RenderCore::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_ALWAYS;
        }

        [[nodiscard]] VkBlendFactor toVkBlendFactor(RenderCore::BlendFactor factor) noexcept
        {
            switch (factor)
            {
                case RenderCore::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
                case RenderCore::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
                case RenderCore::BlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
                case RenderCore::BlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case RenderCore::BlendFactor::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
                case RenderCore::BlendFactor::OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case RenderCore::BlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
                case RenderCore::BlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case RenderCore::BlendFactor::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
                case RenderCore::BlendFactor::OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case RenderCore::BlendFactor::SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            }
            return VK_BLEND_FACTOR_ONE;
        }

        [[nodiscard]] VkBlendOp toVkBlendOp(RenderCore::BlendEquation equation) noexcept
        {
            switch (equation)
            {
                case RenderCore::BlendEquation::Add: return VK_BLEND_OP_ADD;
                case RenderCore::BlendEquation::Subtract: return VK_BLEND_OP_SUBTRACT;
                case RenderCore::BlendEquation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
                case RenderCore::BlendEquation::Minimum: return VK_BLEND_OP_MIN;
                case RenderCore::BlendEquation::Maximum: return VK_BLEND_OP_MAX;
            }
            return VK_BLEND_OP_ADD;
        }

        [[nodiscard]] VkStencilOp toVkStencilOp(RenderCore::StencilOp op) noexcept
        {
            switch (op)
            {
                case RenderCore::StencilOp::Keep: return VK_STENCIL_OP_KEEP;
                case RenderCore::StencilOp::Zero: return VK_STENCIL_OP_ZERO;
                case RenderCore::StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
                case RenderCore::StencilOp::Increment: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case RenderCore::StencilOp::Decrement: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case RenderCore::StencilOp::Invert: return VK_STENCIL_OP_INVERT;
            }
            return VK_STENCIL_OP_KEEP;
        }

        [[nodiscard]] VkSamplerAddressMode toVkAddressMode(RenderCore::TextureWrap wrap) noexcept
        {
            switch (wrap)
            {
                case RenderCore::TextureWrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case RenderCore::TextureWrap::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case RenderCore::TextureWrap::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case RenderCore::TextureWrap::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }

        [[nodiscard]] VkFilter toVkFilter(RenderCore::TextureFilter filter) noexcept
        {
            return filter == RenderCore::TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        }

        [[nodiscard]] VkSamplerMipmapMode toVkMipmapMode(RenderCore::TextureMipmapMode mode) noexcept
        {
            return mode == RenderCore::TextureMipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                                  : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }

        [[nodiscard]] vsg::dmat4 toVsg(const glm::mat4& source) noexcept
        {
            vsg::dmat4 result;
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                    result(static_cast<unsigned int>(column), static_cast<unsigned int>(row)) = source[column][row];
            }
            return result;
        }

        [[nodiscard]] vsg::vec4 toVsg(const glm::vec4& value) noexcept
        {
            return { value.x, value.y, value.z, value.w };
        }

        [[nodiscard]] bool hasNonIdentityTextureTransform(const RenderCore::TextureTransform& transform) noexcept
        {
            return transform.offset.x != 0.0f || transform.offset.y != 0.0f || transform.scale.x != 1.0f
                || transform.scale.y != 1.0f || transform.center.x != 0.5f || transform.center.y != 0.5f
                || transform.rotation != 0.0f
                || transform.convention != RenderCore::TextureTransformConvention::Direct;
        }

        [[nodiscard]] StaticMaterialShaderFamily selectShaderFamily(const RenderCore::MaterialRecord&) noexcept
        {
            // CP3B3 NIF translation publishes legacy fixed-function/Gamebryo
            // semantics. Do not guess a modern PBR model from texture names or
            // source provenance. CP4+ may explicitly select ModernPbr when a
            // producer owns a true metallic/roughness material contract.
            return StaticMaterialShaderFamily::LegacyCompatibility;
        }

        [[nodiscard]] std::optional<const char*> descriptorName(RenderCore::TextureRole role) noexcept
        {
            switch (role)
            {
                case RenderCore::TextureRole::Diffuse: return "diffuseMap";
                case RenderCore::TextureRole::Dark: return "darkMap";
                case RenderCore::TextureRole::Detail: return "detailMap";
                case RenderCore::TextureRole::Decal: return "openmwDecalMap";
                case RenderCore::TextureRole::Emissive: return "emissiveMap";
                case RenderCore::TextureRole::Normal: return "normalMap";
                case RenderCore::TextureRole::Specular: return "specularMap";
                case RenderCore::TextureRole::Bump: return "openmwBumpMap";
                case RenderCore::TextureRole::Gloss: return "openmwGlossMap";
                case RenderCore::TextureRole::Blend: return "openmwTerrainBlendMap";
                case RenderCore::TextureRole::Environment: return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] const char* textureRoleName(RenderCore::TextureRole role) noexcept
        {
            switch (role)
            {
                case RenderCore::TextureRole::Diffuse: return "Diffuse";
                case RenderCore::TextureRole::Dark: return "Dark";
                case RenderCore::TextureRole::Detail: return "Detail";
                case RenderCore::TextureRole::Decal: return "Decal";
                case RenderCore::TextureRole::Emissive: return "Emissive";
                case RenderCore::TextureRole::Normal: return "Normal";
                case RenderCore::TextureRole::Specular: return "Specular";
                case RenderCore::TextureRole::Environment: return "Environment";
                case RenderCore::TextureRole::Bump: return "Bump";
                case RenderCore::TextureRole::Gloss: return "Gloss";
                case RenderCore::TextureRole::Blend: return "Blend";
            }
            return "Unknown";
        }

        [[nodiscard]] vsg::ref_ptr<vsg::Sampler> createSampler(const RenderCore::SamplerRealizationKey& key)
        {
            auto sampler = vsg::Sampler::create();
            sampler->minFilter = toVkFilter(key.minFilter);
            sampler->magFilter = toVkFilter(key.magFilter);
            sampler->mipmapMode = toVkMipmapMode(key.mipmapMode);
            sampler->addressModeU = toVkAddressMode(key.wrapU);
            sampler->addressModeV = toVkAddressMode(key.wrapV);
            sampler->addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler->anisotropyEnable = key.maxAnisotropyBits != 0u ? VK_TRUE : VK_FALSE;
            sampler->maxAnisotropy = std::bit_cast<float>(key.maxAnisotropyBits);
            sampler->minLod = 0.0f;
            sampler->maxLod = key.mipmapMode == RenderCore::TextureMipmapMode::None ? 0.0f : VK_LOD_CLAMP_NONE;
            return sampler;
        }

        struct PipelineStateVisitor final : vsg::Visitor
        {
            const RenderCore::GraphicsPipelineKey& key;

            explicit PipelineStateVisitor(const RenderCore::GraphicsPipelineKey& source)
                : key(source)
            {
            }

            void apply(vsg::Object& object) override { object.traverse(*this); }

            void apply(vsg::InputAssemblyState& state) override
            {
                state.topology = toVkTopology(key.topology);
                state.primitiveRestartEnable
                    = key.topology == RenderCore::PrimitiveTopology::TriangleStrip ? VK_TRUE : VK_FALSE;
            }

            void apply(vsg::RasterizationState& state) override
            {
                state.cullMode = toVkCull(key.fixedFunction.raster.cullMode);
                state.frontFace = toVkFrontFace(key.fixedFunction.raster.frontFace);
                state.polygonMode = key.fixedFunction.raster.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                state.depthBiasEnable = key.fixedFunction.raster.decal ? VK_TRUE : VK_FALSE;
                if (state.depthBiasEnable)
                {
                    state.depthBiasConstantFactor = 1.0f;
                    state.depthBiasSlopeFactor = 1.0f;
                }
            }

            void apply(vsg::ColorBlendState& state) override
            {
                if (state.attachments.empty())
                    state.attachments.emplace_back();
                auto& attachment = state.attachments.front();
                attachment.blendEnable = key.fixedFunction.blend.enabled ? VK_TRUE : VK_FALSE;
                attachment.srcColorBlendFactor = toVkBlendFactor(key.fixedFunction.blend.source);
                attachment.dstColorBlendFactor = toVkBlendFactor(key.fixedFunction.blend.destination);
                attachment.colorBlendOp = toVkBlendOp(key.fixedFunction.blend.equation);
                attachment.srcAlphaBlendFactor = toVkBlendFactor(key.fixedFunction.blend.sourceAlpha);
                attachment.dstAlphaBlendFactor = toVkBlendFactor(key.fixedFunction.blend.destinationAlpha);
                attachment.alphaBlendOp = attachment.colorBlendOp;
                attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            }

            void apply(vsg::DepthStencilState& state) override
            {
                state.depthTestEnable = key.fixedFunction.depthStencil.depthTest ? VK_TRUE : VK_FALSE;
                state.depthWriteEnable = key.fixedFunction.depthStencil.depthWrite ? VK_TRUE : VK_FALSE;
                state.depthCompareOp = key.fixedFunction.depthStencil.equalDepth
                    ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
                state.stencilTestEnable = key.fixedFunction.depthStencil.stencilEnabled ? VK_TRUE : VK_FALSE;
                if (state.stencilTestEnable)
                {
                    VkStencilOpState stencil{};
                    stencil.failOp = toVkStencilOp(key.fixedFunction.depthStencil.stencilFail);
                    stencil.passOp = toVkStencilOp(key.fixedFunction.depthStencil.stencilPass);
                    stencil.depthFailOp = toVkStencilOp(key.fixedFunction.depthStencil.stencilDepthFail);
                    stencil.compareOp = toVkCompare(key.fixedFunction.depthStencil.stencilCompare);
                    stencil.compareMask = key.fixedFunction.depthStencil.stencilCompareMask;
                    stencil.writeMask = std::numeric_limits<std::uint32_t>::max();
                    stencil.reference = key.fixedFunction.depthStencil.stencilReference;
                    state.front = stencil;
                    state.back = stencil;
                }
            }
        };

        [[nodiscard]] vsg::ref_ptr<vsg::TexCoordIndicesValue> makeTexCoordIndices(
            const RenderCore::MaterialRecord& source)
        {
            auto result = vsg::TexCoordIndicesValue::create();
            auto& indices = result->value();
            for (const RenderCore::TextureBinding& binding : source.textures)
            {
                const std::int32_t uv = static_cast<std::int32_t>(binding.transform.uvSet);
                switch (binding.role)
                {
                    case RenderCore::TextureRole::Diffuse: indices.diffuseMap = uv; break;
                    case RenderCore::TextureRole::Detail: indices.detailMap = uv; break;
                    case RenderCore::TextureRole::Normal: indices.normalMap = uv; break;
                    case RenderCore::TextureRole::Emissive: indices.emissiveMap = uv; break;
                    case RenderCore::TextureRole::Specular: indices.specularMap = uv; break;
                    default: break;
                }
            }
            return result;
        }

        [[nodiscard]] vsg::dsphere drawBound(const RenderCore::MeshPayload& mesh, const glm::mat4& world)
        {
            if (mesh.positions.empty())
                return {};
            glm::dvec3 minimum(std::numeric_limits<double>::max());
            glm::dvec3 maximum(std::numeric_limits<double>::lowest());
            for (const glm::vec3& position : mesh.positions)
            {
                const glm::dvec4 p = glm::dmat4(world) * glm::dvec4(position, 1.0);
                minimum = glm::min(minimum, glm::dvec3(p));
                maximum = glm::max(maximum, glm::dvec3(p));
            }
            const glm::dvec3 center = (minimum + maximum) * 0.5;
            const double radius = glm::length(maximum - minimum) * 0.5;
            return { center.x, center.y, center.z, radius };
        }

        struct TextureCacheEntry
        {
            vsg::ref_ptr<vsg::Data> data;
        };
    }

    bool updateDeformedAssetRealization(const RenderCore::RenderWorld& world,
        const StaticAssetPlan& plan, const MeshPayloadResolver& resolve,
        std::vector<StaticRealizationResult::MutableDrawStreams>& streams)
    {
        if (streams.size() != plan.draws.size())
            return false;
        std::vector<const RenderCore::MeshPayload*> payloads;
        payloads.reserve(plan.draws.size());
        for (std::size_t i = 0; i < plan.draws.size(); ++i)
        {
            const auto& draw = plan.draws[i];
            const auto* mesh = world.get(draw.mesh);
            const auto* payload = resolve ? resolve(draw.mesh, draw.node) : nullptr;
            if (!payload && mesh)
                payload = mesh->payload.get();
            const auto& target = streams[i];
            if (!payload || !target.positions || !target.normals || !target.transform
                || target.positions->size() != payload->positions.size()
                || target.normals->size() != payload->positions.size()
                || (!payload->normals.empty() && payload->normals.size() != payload->positions.size()))
                return false;
            for (const auto& p : payload->positions)
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    return false;
            for (const auto& n : payload->normals)
                if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z))
                    return false;
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    if (!std::isfinite(draw.worldTransform[col][row]))
                        return false;
            payloads.push_back(payload);
        }
        for (std::size_t i = 0; i < streams.size(); ++i)
        {
            auto& target = streams[i];
            const auto& payload = *payloads[i];
            bool positionsChanged = false, normalsChanged = false;
            for (std::size_t vertex = 0; vertex < payload.positions.size(); ++vertex)
            {
                const auto& p = payload.positions[vertex];
                const vsg::vec3 position(p.x, p.y, p.z);
                const auto normal = payload.normals.empty() ? glm::vec3(0, 0, 1) : payload.normals[vertex];
                const vsg::vec3 n(normal.x, normal.y, normal.z);
                if ((*target.positions)[vertex] != position)
                {
                    (*target.positions)[vertex] = position;
                    positionsChanged = true;
                }
                if ((*target.normals)[vertex] != n)
                {
                    (*target.normals)[vertex] = n;
                    normalsChanged = true;
                }
            }
            if (positionsChanged)
                target.positions->dirty();
            if (normalsChanged)
                target.normals->dirty();
            target.transform->matrix = toVsg(plan.draws[i].worldTransform);
            if (target.sorted)
                target.sorted->bound = drawBound(payload, plan.draws[i].worldTransform);
        }
        return true;
    }

    StaticAssetRealizer::StaticAssetRealizer(vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
        : mSharedObjects(sharedObjects ? std::move(sharedObjects) : vsg::SharedObjects::create())
    {
    }

    StaticRealizationResult StaticAssetRealizer::realize(const RenderCore::RenderWorld& world,
        const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver,
        const MeshPayloadResolver& meshPayloadResolver,
        std::span<const RenderCore::PopulationInstanceRecord> placements, glm::dvec3 placementOrigin,
        float opacityMultiplier, bool dynamicData) const
    {
        using namespace RenderCore;

        StaticRealizationResult result;
        result.root = vsg::Group::create();

        std::unordered_set<GraphicsPipelineKey, GraphicsPipelineKeyHash> pipelineKeys;
        std::unordered_set<MaterialRealizationKey, MaterialRealizationKeyHash> materialKeys;
        std::unordered_set<TextureRealizationKey, TextureRealizationKeyHash> textureKeys;
        std::unordered_set<SamplerRealizationKey, SamplerRealizationKeyHash> samplerKeys;
        std::unordered_map<TextureRealizationKey, TextureCacheEntry, TextureRealizationKeyHash> textureCache;
        std::unordered_map<SamplerRealizationKey, vsg::ref_ptr<vsg::Sampler>, SamplerRealizationKeyHash> samplerCache;

        // Runtime realization is owner-thread work. Keep compiled shader
        // variants and immutable pipelines across per-frame sharing arenas.
        // Thread-local ownership also isolates independent fixture/worker use.
        static thread_local PersistentShaderFamilies persistentShaders;
        static thread_local PersistentPipelineCache persistentPipelines;
        const bool reuseShaders = std::getenv("OPENMW_V4_PERSISTENT_SHADERS") != nullptr;
        const bool reusePipelines = std::getenv("OPENMW_V4_PERSISTENT_PIPELINES") != nullptr;
        auto legacyShaderSet = reuseShaders ? persistentShaders.get(false, false, false)
                                            : createLegacyCompatibilityShaderSet();
        if (!legacyShaderSet)
        {
            result.root = {};
            result.diagnostics.emplace_back("OpenMW legacy compatibility ShaderSet is unavailable");
            return result;
        }
        vsg::ref_ptr<vsg::ShaderSet> legacyBumpShaderSet;
        vsg::ref_ptr<vsg::ShaderSet> enchantedShaderSet;
        vsg::ref_ptr<vsg::ShaderSet> enchantedBumpShaderSet;
        vsg::ref_ptr<vsg::ShaderSet> sphereShaderSet;
        vsg::ref_ptr<vsg::ShaderSet> sphereBumpShaderSet;

        vsg::ref_ptr<vsg::vec3Array> instanceTranslations;
        vsg::ref_ptr<vsg::vec4Array> instanceRotations;
        vsg::ref_ptr<vsg::vec3Array> instanceScales;
        if (!placements.empty())
        {
            if (placements.size() > std::numeric_limits<std::uint32_t>::max())
            {
                result.root = {};
                result.diagnostics.emplace_back("Static population exceeds Vulkan draw instanceCount range");
                return result;
            }
            instanceTranslations = vsg::vec3Array::create(placements.size());
            instanceRotations = vsg::vec4Array::create(placements.size());
            instanceScales = vsg::vec3Array::create(placements.size());
            for (std::size_t i = 0; i < placements.size(); ++i)
            {
                const RenderCore::WorldTransform& placement = placements[i].transform;
                const glm::dvec3 relative = placement.translation - placementOrigin;
                instanceTranslations->set(i, vsg::vec3(
                    static_cast<float>(relative.x), static_cast<float>(relative.y), static_cast<float>(relative.z)));
                instanceRotations->set(i, vsg::vec4(placement.rotation.x, placement.rotation.y,
                    placement.rotation.z, placement.rotation.w));
                instanceScales->set(i, vsg::vec3(placement.scale.x, placement.scale.y, placement.scale.z));
            }
        }

        for (const StaticDrawPlan& draw : plan.draws)
        {
            const MeshRecord* mesh = world.get(draw.mesh);
            const MaterialRecord* publishedMaterial = world.get(draw.material);
            if (!std::isfinite(opacityMultiplier) || opacityMultiplier < 0.0f || opacityMultiplier > 1.0f)
            {
                result.root = {};
                result.diagnostics.emplace_back("Actor opacity multiplier is outside the normalized range");
                return result;
            }
            MaterialRecord fadedMaterial;
            const MaterialRecord* material = publishedMaterial;
            GraphicsPipelineKey effectivePipeline = draw.pipeline;
            if (publishedMaterial && opacityMultiplier < 1.0f)
            {
                fadedMaterial = *publishedMaterial;
                fadedMaterial.alpha *= opacityMultiplier;
                fadedMaterial.alphaBlendEnabled = true;
                fadedMaterial.alphaMode = AlphaMode::Blend;
                fadedMaterial.sourceBlend = BlendFactor::SourceAlpha;
                fadedMaterial.destinationBlend = BlendFactor::OneMinusSourceAlpha;
                fadedMaterial.blendEquation = BlendEquation::Add;
                fadedMaterial.transparentSort = TransparentSortPolicy::Sorted;
                material = &fadedMaterial;
                effectivePipeline.fixedFunction.blend.enabled = true;
                effectivePipeline.fixedFunction.blend.source = BlendFactor::SourceAlpha;
                effectivePipeline.fixedFunction.blend.destination = BlendFactor::OneMinusSourceAlpha;
                effectivePipeline.fixedFunction.blend.equation = BlendEquation::Add;
            }
            const MeshPayload* resolvedPayload
                = meshPayloadResolver ? meshPayloadResolver(draw.mesh, draw.node) : nullptr;
            if (!resolvedPayload && mesh)
                resolvedPayload = mesh->payload.get();
            if (!mesh || !resolvedPayload || !validMeshPayload(*resolvedPayload) || !material
                || draw.surfaceIndex >= resolvedPayload->surfaces.size())
            {
                result.root = {};
                result.diagnostics.emplace_back("Static draw references a missing published mesh/material resource");
                return result;
            }

            const StaticMaterialShaderFamily shaderFamily = selectShaderFamily(*material);
            if (shaderFamily != StaticMaterialShaderFamily::LegacyCompatibility)
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Modern PBR static material family was selected without an explicit CP4+ material contract");
                return result;
            }
            ++result.stats.legacyCompatibilityDraws;

            const MeshPayload& payload = *resolvedPayload;
            // Include static assets as well as evaluated actors/objects. An
            // explicit filter can observe realization during loading, outside
            // sampled gameplay frames, without enabling per-draw logging.
            if (Debug::GameplayDiagnostics::enabled()
                && Debug::RuntimeDiagnostics::mode() == Debug::RuntimeDiagnostics::Mode::Focused
                && Debug::GameplayDiagnostics::context.materialProbes < 8
                && [&] {
                    const auto filter = Debug::GameplayDiagnostics::materialProbeFilter();
                    if (filter.empty()) return Debug::GameplayDiagnostics::sampling();
                    if (mesh->sourceIdentity.find(filter) != std::string::npos
                        || material->sourceIdentity.find(filter) != std::string::npos) return true;
                    return std::any_of(material->textures.begin(), material->textures.end(), [&](const auto& binding) {
                        const auto* texture = world.get(binding.texture);
                        return texture && texture->sourceIdentity.find(filter) != std::string::npos;
                    });
                }())
            {
                ++Debug::GameplayDiagnostics::context.materialProbes;
                const auto vectorText = [](const auto& value, unsigned size) {
                    std::string out;
                    for (unsigned i = 0; i < size; ++i) { if (i) out += ','; out += std::to_string(value[i]); }
                    return out;
                };
                Debug::GameplayDiagnostics::recordEvent("material_probe", {
                    {"phase", "realization"}, {"mesh", mesh->sourceIdentity}, {"draw", material->sourceIdentity},
                    {"environment_mode", std::to_string(static_cast<unsigned>(material->environmentMapMode))},
                    {"environment_color", vectorText(material->environmentMapColor, 4)},
                    {"environment_pre_light", std::to_string(material->environmentMapPreLight)},
                    {"specular", vectorText(material->specular, 4)}, {"shininess", std::to_string(material->shininess)},
                    {"bump_matrix", vectorText(material->bumpMapMatrix, 4)},
                    {"bump_luma", vectorText(material->environmentMapLumaBias, 2)}}, true);
                for (std::size_t i = 0; i < std::min(material->textures.size(), std::size_t{8}); ++i)
                {
                    const auto& binding = material->textures[i];
                    const auto* texture = world.get(binding.texture);
                    if (!texture) continue;
                    Debug::GameplayDiagnostics::recordEvent("texture_probe", {
                        {"phase", "realization"}, {"draw", material->sourceIdentity}, {"source", texture->sourceIdentity},
                        {"content", texture->contentIdentity}, {"role", std::to_string(static_cast<unsigned>(binding.role))},
                        {"color_space", std::to_string(static_cast<unsigned>(binding.colorSpace))},
                        {"uv_set", std::to_string(binding.transform.uvSet)},
                        {"uv_scale", vectorText(binding.transform.scale, 2)},
                        {"uv_offset", vectorText(binding.transform.offset, 2)},
                        {"width", std::to_string(texture->width)}, {"height", std::to_string(texture->height)}}, true);
                }
            }
            if (payload.texCoordSets.size() > 4u)
            {
                result.root = {};
                std::string diagnostic
                    = "VSG 1.1.15 standard compatibility vertex contract exposes four texture-coordinate sets"
                    " [mesh='" + mesh->sourceIdentity + "', material='" + material->sourceIdentity
                    + "', uv_streams=" + std::to_string(payload.texCoordSets.size())
                    + ", texture_bindings=" + std::to_string(material->textures.size()) + "]";
                // Failure provenance must not depend on the ordinary per-frame
                // diagnostic example budget. Report bindings without dropping
                // textures or aliasing genuinely distinct source coordinates.
                const std::size_t limit = (std::min)(material->textures.size(), std::size_t{ 16 });
                for (std::size_t index = 0; index < limit; ++index)
                {
                    const TextureBinding& binding = material->textures[index];
                    const TextureRecord* texture = world.get(binding.texture);
                    diagnostic += " [stage=" + std::to_string(index)
                        + ", role=" + std::to_string(static_cast<unsigned int>(binding.role))
                        + ", uv=" + std::to_string(binding.transform.uvSet)
                        + ", texture='" + (texture ? texture->sourceIdentity : std::string("missing")) + "']";
                }
                if (material->textures.size() > limit)
                    diagnostic += " [additional_bindings=" + std::to_string(material->textures.size() - limit) + "]";
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }

            const bool usesVertexColors = material->vertexColorMode != VertexColorMode::Ignore;
            if (usesVertexColors && payload.colors.empty())
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Legacy material requires authored vertex colors but the published mesh has no color stream");
                return result;
            }

            const std::size_t bumpBindingCount = static_cast<std::size_t>(std::count_if(
                material->textures.begin(), material->textures.end(),
                [](const TextureBinding& binding) { return binding.role == TextureRole::Bump; }));
            if (bumpBindingCount > 1u || material->bumpParametersEnabled != (bumpBindingCount == 1u))
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Legacy bump metadata does not match V3.25's single NiTexturingProperty BumpTexture contract"
                    " [mesh='" + mesh->sourceIdentity + "', material='" + material->sourceIdentity
                    + "', bump_bindings=" + std::to_string(bumpBindingCount) + ", parameters_enabled="
                    + std::to_string(material->bumpParametersEnabled) + "]");
                return result;
            }
            const bool legacyBump = material->bumpParametersEnabled;
            if (legacyBump)
            {
                const auto bumpBinding = std::find_if(material->textures.begin(), material->textures.end(),
                    [](const TextureBinding& binding) { return binding.role == TextureRole::Bump; });
                if (bumpBinding == material->textures.end() || bumpBinding->transform.uvSet >= payload.texCoordSets.size())
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        "Legacy NiTexturingProperty bump stage references a texture-coordinate set absent from the published mesh");
                    return result;
                }
            }

            const std::size_t environmentBindingCount = static_cast<std::size_t>(std::count_if(
                material->textures.begin(), material->textures.end(),
                [](const TextureBinding& binding) { return binding.role == TextureRole::Environment; }));
            const bool enchantedEnvironment
                = material->environmentMapMode == EnvironmentMapMode::EnchantedSequence
                && environmentBindingCount == EnchantedEnvironmentFrameCount;
            const bool sphereEnvironment = material->environmentMapMode == EnvironmentMapMode::SphereMap
                && environmentBindingCount == 1u
                && !std::getenv("OPENMW_V4_LEGACY_AUTHORED_ENVIRONMENT_CONTROL");
            const bool environment = enchantedEnvironment || sphereEnvironment;
            if (!environment && (environmentBindingCount != 0u
                    || material->environmentMapMode != EnvironmentMapMode::None))
            {
                result.root = {};
                result.stats.unsupportedTextureBindings += static_cast<std::uint32_t>(environmentBindingCount);
                result.diagnostics.emplace_back(
                    "Legacy environment metadata must describe one sphere map or the exact 32-frame enchanted sequence"
                    " [material='" + material->sourceIdentity + "', mode="
                    + std::to_string(static_cast<unsigned>(material->environmentMapMode))
                    + ", bindings=" + std::to_string(environmentBindingCount) + "]");
                return result;
            }

            vsg::ref_ptr<vsg::ShaderSet> shaderSet = legacyShaderSet;
            if (sphereEnvironment)
            {
                if (!sphereShaderSet)
                    sphereShaderSet = reuseShaders ? persistentShaders.get(false, true, false)
                                                   : createEnchantedLegacyCompatibilityShaderSet({}, true);
                if (legacyBump && !sphereBumpShaderSet)
                    sphereBumpShaderSet = reuseShaders ? persistentShaders.get(false, true, true)
                                                       : createLegacyBumpCompatibilityShaderSet(sphereShaderSet);
                shaderSet = legacyBump ? sphereBumpShaderSet : sphereShaderSet;
                if (!shaderSet)
                {
                    result.root = {};
                    result.diagnostics.emplace_back("Legacy sphere-map/bump ShaderSet construction failed");
                    return result;
                }
            }
            else if (enchantedEnvironment)
            {
                if (!enchantedShaderSet)
                    enchantedShaderSet = reuseShaders ? persistentShaders.get(true, false, false)
                                                      : createEnchantedLegacyCompatibilityShaderSet();
                if (!enchantedShaderSet)
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        "OpenMW enchanted legacy compatibility ShaderSet could not be constructed from the pinned VSG contract");
                    return result;
                }
                if (legacyBump)
                {
                    if (!enchantedBumpShaderSet)
                        enchantedBumpShaderSet = reuseShaders ? persistentShaders.get(true, false, true)
                            : createLegacyBumpCompatibilityShaderSet(enchantedShaderSet);
                    if (!enchantedBumpShaderSet)
                    {
                        result.root = {};
                        result.diagnostics.emplace_back(
                            "OpenMW legacy bump+environment ShaderSet could not be constructed from the pinned compatibility contract");
                        return result;
                    }
                    shaderSet = enchantedBumpShaderSet;
                }
                else
                    shaderSet = enchantedShaderSet;
            }
            else if (legacyBump)
            {
                if (!legacyBumpShaderSet)
                    legacyBumpShaderSet = reuseShaders ? persistentShaders.get(false, false, true)
                                                       : createLegacyBumpCompatibilityShaderSet(legacyShaderSet);
                if (!legacyBumpShaderSet)
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        "OpenMW legacy bump ShaderSet could not be constructed from the pinned compatibility contract");
                    return result;
                }
                shaderSet = legacyBumpShaderSet;
            }

            auto config = vsg::GraphicsPipelineConfigurator::create(shaderSet);
            if (!config)
            {
                result.root = {};
                result.diagnostics.emplace_back("Failed to allocate legacy GraphicsPipelineConfigurator");
                return result;
            }

            vsg::DataList arrays;
            auto positions = vsg::vec3Array::create(payload.positions.size());
            if (dynamicData)
                positions->properties.dataVariance = vsg::DYNAMIC_DATA;
            for (std::size_t i = 0; i < payload.positions.size(); ++i)
                positions->set(i, vsg::vec3(payload.positions[i].x, payload.positions[i].y, payload.positions[i].z));
            if (!config->assignArray(arrays, "vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, positions))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader does not expose vsg_Vertex");
                return result;
            }
            if (instanceTranslations
                && (!config->assignArray(
                        arrays, "vsg_Translation", VK_VERTEX_INPUT_RATE_INSTANCE, instanceTranslations)
                    || !config->assignArray(arrays, "vsg_Rotation", VK_VERTEX_INPUT_RATE_INSTANCE, instanceRotations)
                    || !config->assignArray(arrays, "vsg_Scale", VK_VERTEX_INPUT_RATE_INSTANCE, instanceScales)))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader rejected CP4C instance transforms");
                return result;
            }

            auto normals = vsg::vec3Array::create(payload.positions.size());
            if (dynamicData)
                normals->properties.dataVariance = vsg::DYNAMIC_DATA;
            for (std::size_t i = 0; i < payload.positions.size(); ++i)
            {
                const glm::vec3 normal = payload.normals.empty() ? glm::vec3(0.0f, 0.0f, 1.0f) : payload.normals[i];
                normals->set(i, vsg::vec3(normal.x, normal.y, normal.z));
            }
            config->assignArray(arrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, normals);

            std::vector<vsg::ref_ptr<vsg::vec2Array>> mutableTexCoords;
            mutableTexCoords.reserve(payload.texCoordSets.size());
            for (std::size_t set = 0; set < payload.texCoordSets.size(); ++set)
            {
                auto texCoords = vsg::vec2Array::create(payload.texCoordSets[set].size());
                if (dynamicData)
                    texCoords->properties.dataVariance = vsg::DYNAMIC_DATA;
                for (std::size_t i = 0; i < payload.texCoordSets[set].size(); ++i)
                    texCoords->set(i, vsg::vec2(payload.texCoordSets[set][i].x, payload.texCoordSets[set][i].y));
                config->assignArray(arrays, "vsg_TexCoord" + std::to_string(set), VK_VERTEX_INPUT_RATE_VERTEX, texCoords);
                mutableTexCoords.push_back(std::move(texCoords));
            }

            auto colors = vsg::vec4Array::create(payload.positions.size());
            if (dynamicData)
                colors->properties.dataVariance = vsg::DYNAMIC_DATA;
            for (std::size_t i = 0; i < payload.positions.size(); ++i)
            {
                const glm::vec4 color = usesVertexColors ? payload.colors[i] : glm::vec4(1.0f);
                colors->set(i, toVsg(color));
            }
            if (!config->assignArray(arrays, "vsg_Color", VK_VERTEX_INPUT_RATE_VERTEX, colors))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader rejected its explicit vertex color stream");
                return result;
            }

            auto materialUniform = makeLegacyCompatibilityMaterial(*material);
            if (!config->assignDescriptor("material", materialUniform))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader rejected the OpenMW material descriptor");
                return result;
            }
            config->assignDescriptor("texCoordIndices", makeTexCoordIndices(*material));
            if (legacyBump
                && !config->assignDescriptor("openmwLegacyBump", makeLegacyBumpMaterial(*material)))
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Legacy bump compatibility shader rejected the authored bump-matrix/luma descriptor");
                return result;
            }

            if (std::ranges::any_of(material->textures,
                    [](const TextureBinding& binding) { return hasNonIdentityTextureTransform(binding.transform); }))
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back(
                    "Static texture transform is preserved in RenderCore but requires a dedicated compatibility shader variant");
            }
            if (material->treeAnimation)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back("Legacy static material requires tree-animation compatibility semantics");
            }
            if (material->refraction)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back("Legacy static material requires refraction compatibility semantics");
            }
            if (material->softEffect)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back("Legacy static material requires soft-effect compatibility semantics");
            }
            if (material->falloff)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back("Legacy static material requires falloff compatibility semantics");
            }

            if (draw.billboard)
                ++result.stats.billboardDraws;

            const auto realizeBinding = [&](std::size_t bindingIndex, vsg::ref_ptr<vsg::Data>& data,
                                            vsg::ref_ptr<vsg::Sampler>& sampler) -> bool {
                if (bindingIndex >= draw.textures.size() || bindingIndex >= draw.samplers.size())
                {
                    result.root = {};
                    result.diagnostics.emplace_back("Static plan texture/sampler arrays do not match material binding order");
                    return false;
                }

                const TextureRealizationKey& textureKey = draw.textures[bindingIndex];
                const SamplerRealizationKey& samplerKey = draw.samplers[bindingIndex];
                const TextureRecord* texture = world.get(textureKey.view.texture);
                if (!texture)
                {
                    result.root = {};
                    result.diagnostics.emplace_back("Published texture vanished before VSG realization");
                    return false;
                }

                if (auto found = textureCache.find(textureKey); found != textureCache.end())
                {
                    data = found->second.data;
                    ++result.stats.textureCacheHits;
                }
                else
                {
                    data = textureResolver ? textureResolver(*texture, textureKey) : vsg::ref_ptr<vsg::Data>{};
                    if (!data)
                    {
                        result.root = {};
                        std::ostringstream message;
                        message << "Texture resolver failed for published texture handle slot="
                                << textureKey.view.texture.slot();
                        result.diagnostics.push_back(message.str());
                        return false;
                    }
                    textureCache.emplace(textureKey, TextureCacheEntry{ data });
                    ++result.stats.textureLoads;
                }

                if (auto found = samplerCache.find(samplerKey); found != samplerCache.end())
                    sampler = found->second;
                else
                {
                    sampler = createSampler(samplerKey);
                    mSharedObjects->share(sampler);
                    samplerCache.emplace(samplerKey, sampler);
                }

                textureKeys.insert(textureKey);
                samplerKeys.insert(samplerKey);
                return true;
            };

            if (environment)
            {
                if (!config->assignDescriptor(
                        "openmwEnvironmentEffect", makeEnchantedEnvironmentMaterial(*material)))
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        "Enchanted compatibility shader rejected its environment color descriptor");
                    return result;
                }

                vsg::ImageInfoList frames;
                frames.reserve(environmentBindingCount);
                for (std::size_t bindingIndex = 0; bindingIndex < material->textures.size(); ++bindingIndex)
                {
                    if (material->textures[bindingIndex].role != TextureRole::Environment)
                        continue;
                    vsg::ref_ptr<vsg::Data> data;
                    vsg::ref_ptr<vsg::Sampler> sampler;
                    if (!realizeBinding(bindingIndex, data, sampler))
                        return result;
                    frames.push_back(liveTextureImage(data, sampler));
                }
                if (frames.size() != environmentBindingCount
                    || !config->assignTexture("openmwEnvironmentMaps", frames))
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        "Legacy compatibility shader rejected its environment image descriptors");
                    return result;
                }
            }

            for (std::size_t bindingIndex = 0; bindingIndex < material->textures.size(); ++bindingIndex)
            {
                const TextureBinding& binding = material->textures[bindingIndex];
                if (binding.role == TextureRole::Blend && (!material->terrainLayer
                        || binding.transform.uvSet != 1 || payload.texCoordSets.size() < 2))
                {
                    result.root = {};
                    result.diagnostics.emplace_back("LAND blend binding requires a terrain layer and UV set 1");
                    return result;
                }
                if (environment && binding.role == TextureRole::Environment)
                    continue;
                const auto descriptor = descriptorName(binding.role);
                if (!descriptor)
                {
                    ++result.stats.unsupportedTextureBindings;
                    if (binding.role != TextureRole::Environment)
                    {
                        result.diagnostics.emplace_back(std::string("Legacy static texture role remains fail-closed: ")
                            + textureRoleName(binding.role));
                    }
                    continue;
                }

                vsg::ref_ptr<vsg::Data> data;
                vsg::ref_ptr<vsg::Sampler> sampler;
                if (!realizeBinding(bindingIndex, data, sampler))
                    return result;
                if (binding.role == TextureRole::Normal
                    && (material->terrainLayer || !std::getenv("OPENMW_V4_LEGACY_NORMAL_MAPPING_CONTROL"))
                    && (data->properties.format == VK_FORMAT_BC5_UNORM_BLOCK
                        || data->properties.format == VK_FORMAT_R8G8_UNORM
                        || data->properties.format == VK_FORMAT_R16G16_UNORM))
                    materialUniform->value().textureCoordSets.w += 8.f;
                if (!config->assignTexture(*descriptor, vsg::ImageInfoList{ liveTextureImage(data, sampler) }))
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        std::string("Legacy compatibility ShaderSet rejected texture descriptor ") + *descriptor);
                    return result;
                }
            }

            PipelineStateVisitor stateVisitor(effectivePipeline);
            config->accept(stateVisitor);
            mSharedObjects->share(config, [](const vsg::ref_ptr<vsg::GraphicsPipelineConfigurator>& shared) {
                shared->init();
                shared->graphicsPipeline->setValue("openmw.pipeline.family", "legacy-static");
                shared->graphicsPipeline->setValue("openmw.pipeline.source", "shared legacy compatibility pipeline");
                ViewPipelineBinding::prepareCache(*shared->graphicsPipeline);
            });

            auto stateGroup = vsg::StateGroup::create();
            if (!config->copyTo(stateGroup, mSharedObjects))
            {
                result.root = {};
                result.diagnostics.emplace_back("GraphicsPipelineConfigurator could not copy state into draw StateGroup");
                return result;
            }
            for (vsg::ref_ptr<vsg::StateCommand>& stateCommand : stateGroup->stateCommands)
            {
                auto bindPipeline = stateCommand.cast<vsg::BindGraphicsPipeline>();
                if (!bindPipeline || !bindPipeline->pipeline)
                    continue;
                // copyTo already interns the immutable pipeline independently
                // of the per-draw arrays. Do not mutate an interned binding or
                // its comparison keys after that sharing boundary.
                auto pipeline = reusePipelines ? persistentPipelines.get(bindPipeline->pipeline)
                                               : bindPipeline->pipeline;
                auto viewBinding = ViewPipelineBinding::create(std::move(pipeline));
                mSharedObjects->share(viewBinding);
                stateCommand = viewBinding;
                stateGroup->setValue("openmw.draw.source",
                    mesh->sourceIdentity + " | " + material->sourceIdentity);
            }
            stateGroup->prototypeArrayState = config->getSuitableArrayState();

            auto indices = vsg::uintArray::create(payload.indices.size());
            for (std::size_t i = 0; i < payload.indices.size(); ++i)
                indices->set(i, payload.indices[i]);

            auto command = vsg::VertexIndexDraw::create();
            command->assignArrays(arrays);
            command->assignIndices(indices);
            command->indexCount = draw.surface.indexCount;
            command->instanceCount
                = placements.empty() ? 1u : static_cast<std::uint32_t>(placements.size());
            command->firstIndex = draw.surface.firstIndex;
            stateGroup->addChild(command);

            auto transform = vsg::MatrixTransform::create(toVsg(draw.worldTransform));
            transform->addChild(stateGroup);

            vsg::ref_ptr<vsg::Node> node = transform;
            vsg::ref_ptr<vsg::DepthSorted> sorted;
            if (material->transparentSort == TransparentSortPolicy::Sorted)
            {
                sorted = vsg::DepthSorted::create(10, drawBound(payload, draw.worldTransform), transform);
                node = sorted;
                ++result.stats.sortedDrawCount;
            }
            if (dynamicData)
                result.mutableDraws.push_back({ std::move(positions), std::move(normals),
                    std::move(mutableTexCoords), std::move(colors), transform, sorted });
            result.root->addChild(node);

            pipelineKeys.insert(effectivePipeline);
            materialKeys.insert(draw.materialRealization);
            ++result.stats.drawCount;
        }

        result.stats.pipelineKeys = static_cast<std::uint32_t>(pipelineKeys.size());
        result.stats.materialKeys = static_cast<std::uint32_t>(materialKeys.size());
        result.stats.textureViewKeys = static_cast<std::uint32_t>(textureKeys.size());
        result.stats.samplerKeys = static_cast<std::uint32_t>(samplerKeys.size());
        return result;
    }
}
