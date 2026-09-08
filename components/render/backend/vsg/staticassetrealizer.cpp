#include "legacymaterialshader.hpp"
#include "staticassetrealizer.hpp"

#include <vsg/all.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
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
                case RenderCore::TextureRole::Detail: return "detailMap";
                case RenderCore::TextureRole::Emissive: return "emissiveMap";
                case RenderCore::TextureRole::Normal: return "normalMap";
                case RenderCore::TextureRole::Specular: return "specularMap";
                case RenderCore::TextureRole::Dark:
                case RenderCore::TextureRole::Decal:
                case RenderCore::TextureRole::Environment:
                case RenderCore::TextureRole::Bump:
                case RenderCore::TextureRole::Gloss:
                case RenderCore::TextureRole::Blend: return std::nullopt;
            }
            return std::nullopt;
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
                attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
                attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
                attachment.alphaBlendOp = attachment.colorBlendOp;
                attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            }

            void apply(vsg::DepthStencilState& state) override
            {
                state.depthTestEnable = key.fixedFunction.depthStencil.depthTest ? VK_TRUE : VK_FALSE;
                state.depthWriteEnable = key.fixedFunction.depthStencil.depthWrite ? VK_TRUE : VK_FALSE;
                state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
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

    StaticAssetRealizer::StaticAssetRealizer(vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
        : mSharedObjects(sharedObjects ? std::move(sharedObjects) : vsg::SharedObjects::create())
    {
    }

    StaticRealizationResult StaticAssetRealizer::realize(const RenderCore::RenderWorld& world,
        const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver) const
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

        auto legacyShaderSet = createLegacyCompatibilityShaderSet();
        if (!legacyShaderSet)
        {
            result.root = {};
            result.diagnostics.emplace_back("OpenMW legacy compatibility ShaderSet is unavailable");
            return result;
        }

        for (const StaticDrawPlan& draw : plan.draws)
        {
            const MeshRecord* mesh = world.get(draw.mesh);
            const MaterialRecord* material = world.get(draw.material);
            if (!mesh || !mesh->payload || !material || draw.surfaceIndex >= mesh->payload->surfaces.size())
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

            const MeshPayload& payload = *mesh->payload;
            if (payload.texCoordSets.size() > 4u)
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "VSG 1.1.15 standard compatibility vertex contract exposes four texture-coordinate sets");
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

            auto config = vsg::GraphicsPipelineConfigurator::create(legacyShaderSet);
            if (!config)
            {
                result.root = {};
                result.diagnostics.emplace_back("Failed to allocate legacy GraphicsPipelineConfigurator");
                return result;
            }

            vsg::DataList arrays;
            auto positions = vsg::vec3Array::create(payload.positions.size());
            for (std::size_t i = 0; i < payload.positions.size(); ++i)
                positions->set(i, vsg::vec3(payload.positions[i].x, payload.positions[i].y, payload.positions[i].z));
            if (!config->assignArray(arrays, "vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, positions))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader does not expose vsg_Vertex");
                return result;
            }

            auto normals = vsg::vec3Array::create(payload.positions.size());
            for (std::size_t i = 0; i < payload.positions.size(); ++i)
            {
                const glm::vec3 normal = payload.normals.empty() ? glm::vec3(0.0f, 0.0f, 1.0f) : payload.normals[i];
                normals->set(i, vsg::vec3(normal.x, normal.y, normal.z));
            }
            config->assignArray(arrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, normals);

            for (std::size_t set = 0; set < payload.texCoordSets.size(); ++set)
            {
                auto texCoords = vsg::vec2Array::create(payload.texCoordSets[set].size());
                for (std::size_t i = 0; i < payload.texCoordSets[set].size(); ++i)
                    texCoords->set(i, vsg::vec2(payload.texCoordSets[set][i].x, payload.texCoordSets[set][i].y));
                config->assignArray(arrays, "vsg_TexCoord" + std::to_string(set), VK_VERTEX_INPUT_RATE_VERTEX, texCoords);
            }

            auto colors = vsg::vec4Array::create(payload.positions.size());
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

            if (!config->assignDescriptor("material", makeLegacyCompatibilityMaterial(*material)))
            {
                result.root = {};
                result.diagnostics.emplace_back("Legacy compatibility shader rejected the OpenMW material descriptor");
                return result;
            }
            config->assignDescriptor("texCoordIndices", makeTexCoordIndices(*material));

            if (material->unlit)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back(
                    "Legacy unlit material requires a dedicated compatibility shader variant");
            }
            if (material->textureApply != TextureApplyMode::Modulate)
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back(
                    "Legacy non-Modulate texture apply mode requires a dedicated compatibility shader variant");
            }
            if (std::ranges::any_of(material->textures,
                    [](const TextureBinding& binding) { return hasNonIdentityTextureTransform(binding.transform); }))
            {
                ++result.stats.runtimeContextEffects;
                result.diagnostics.emplace_back(
                    "Static texture transform is preserved in RenderCore but requires a dedicated compatibility shader variant");
            }
            if (material->treeAnimation || material->refraction || material->softEffect || material->falloff
                || material->bumpParametersEnabled)
            {
                ++result.stats.runtimeContextEffects;
            }

            if (draw.billboard)
                ++result.stats.billboardDraws;

            for (std::size_t bindingIndex = 0; bindingIndex < material->textures.size(); ++bindingIndex)
            {
                const TextureBinding& binding = material->textures[bindingIndex];
                const auto descriptor = descriptorName(binding.role);
                if (!descriptor)
                {
                    ++result.stats.unsupportedTextureBindings;
                    continue;
                }
                if (bindingIndex >= draw.textures.size() || bindingIndex >= draw.samplers.size())
                {
                    result.root = {};
                    result.diagnostics.emplace_back("Static plan texture/sampler arrays do not match material binding order");
                    return result;
                }

                const TextureRealizationKey& textureKey = draw.textures[bindingIndex];
                const SamplerRealizationKey& samplerKey = draw.samplers[bindingIndex];
                const TextureRecord* texture = world.get(textureKey.view.texture);
                if (!texture)
                {
                    result.root = {};
                    result.diagnostics.emplace_back("Published texture vanished before VSG realization");
                    return result;
                }

                vsg::ref_ptr<vsg::Data> data;
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
                        message << "Texture resolver failed for published texture handle slot=" << textureKey.view.texture.slot();
                        result.diagnostics.push_back(message.str());
                        return result;
                    }
                    textureCache.emplace(textureKey, TextureCacheEntry{ data });
                    ++result.stats.textureLoads;
                }

                vsg::ref_ptr<vsg::Sampler> sampler;
                if (auto found = samplerCache.find(samplerKey); found != samplerCache.end())
                    sampler = found->second;
                else
                {
                    sampler = createSampler(samplerKey);
                    mSharedObjects->share(sampler);
                    samplerCache.emplace(samplerKey, sampler);
                }

                if (!config->assignTexture(*descriptor, data, sampler))
                {
                    result.root = {};
                    result.diagnostics.emplace_back(
                        std::string("Legacy compatibility ShaderSet rejected texture descriptor ") + *descriptor);
                    return result;
                }

                textureKeys.insert(textureKey);
                samplerKeys.insert(samplerKey);
            }

            PipelineStateVisitor stateVisitor(draw.pipeline);
            config->accept(stateVisitor);
            mSharedObjects->share(config, [](const vsg::ref_ptr<vsg::GraphicsPipelineConfigurator>& shared) { shared->init(); });

            auto stateGroup = vsg::StateGroup::create();
            if (!config->copyTo(stateGroup, mSharedObjects))
            {
                result.root = {};
                result.diagnostics.emplace_back("GraphicsPipelineConfigurator could not copy state into draw StateGroup");
                return result;
            }
            stateGroup->prototypeArrayState = config->getSuitableArrayState();

            auto indices = vsg::uintArray::create(payload.indices.size());
            for (std::size_t i = 0; i < payload.indices.size(); ++i)
                indices->set(i, payload.indices[i]);

            auto command = vsg::VertexIndexDraw::create();
            command->assignArrays(arrays);
            command->assignIndices(indices);
            command->indexCount = draw.surface.indexCount;
            command->instanceCount = 1u;
            command->firstIndex = draw.surface.firstIndex;
            stateGroup->addChild(command);

            auto transform = vsg::MatrixTransform::create(toVsg(draw.worldTransform));
            transform->addChild(stateGroup);

            vsg::ref_ptr<vsg::Node> node = transform;
            if (material->transparentSort == TransparentSortPolicy::Sorted)
            {
                node = vsg::DepthSorted::create(10, drawBound(payload, draw.worldTransform), transform);
                ++result.stats.sortedDrawCount;
            }
            result.root->addChild(node);

            pipelineKeys.insert(draw.pipeline);
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
