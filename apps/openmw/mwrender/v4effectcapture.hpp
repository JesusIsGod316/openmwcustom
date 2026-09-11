#ifndef OPENMW_MWRENDER_V4EFFECTCAPTURE_H
#define OPENMW_MWRENDER_V4EFFECTCAPTURE_H

#include "animation.hpp"

#include <components/rendercore/effectframe.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>

#include <osg/AlphaFunc>
#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/GL>
#include <osg/NodeVisitor>
#include <osg/PolygonMode>
#include <osg/StateSet>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osgParticle/ParticleSystem>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MWRender
{
    struct V4EffectCaptureResult
    {
        std::vector<RenderCore::ImmediateEffectDraw> draws;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return diagnostic.empty(); }
    };

    namespace v4_effect_detail
    {
        [[nodiscard]] inline glm::mat4 toGlm(const osg::Matrixd& source) noexcept
        {
            glm::mat4 result(1.0f);
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    result[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)]
                        = static_cast<float>(source(column, row));
            return result;
        }

        [[nodiscard]] inline glm::vec3 toGlm(const osg::Vec3f& value) noexcept
        {
            return { value.x(), value.y(), value.z() };
        }

        [[nodiscard]] inline glm::vec4 toGlm(const osg::Vec4f& value) noexcept
        {
            return { value.r(), value.g(), value.b(), value.a() };
        }

        [[nodiscard]] inline bool finite(const glm::mat4& value) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
                for (glm::length_t row = 0; row < 4; ++row)
                    if (!std::isfinite(value[column][row]))
                        return false;
            return true;
        }

        [[nodiscard]] inline std::optional<RenderCore::BlendFactor> blendFactor(GLenum value) noexcept
        {
            using RenderCore::BlendFactor;
            switch (value)
            {
                case GL_ONE: return BlendFactor::One;
                case GL_ZERO: return BlendFactor::Zero;
                case GL_SRC_COLOR: return BlendFactor::SourceColor;
                case GL_ONE_MINUS_SRC_COLOR: return BlendFactor::OneMinusSourceColor;
                case GL_DST_COLOR: return BlendFactor::DestinationColor;
                case GL_ONE_MINUS_DST_COLOR: return BlendFactor::OneMinusDestinationColor;
                case GL_SRC_ALPHA: return BlendFactor::SourceAlpha;
                case GL_ONE_MINUS_SRC_ALPHA: return BlendFactor::OneMinusSourceAlpha;
                case GL_DST_ALPHA: return BlendFactor::DestinationAlpha;
                case GL_ONE_MINUS_DST_ALPHA: return BlendFactor::OneMinusDestinationAlpha;
                case GL_SRC_ALPHA_SATURATE: return BlendFactor::SourceAlphaSaturate;
                default: return std::nullopt;
            }
        }

        [[nodiscard]] inline std::optional<RenderCore::CompareOp> compareOp(GLenum value) noexcept
        {
            using RenderCore::CompareOp;
            switch (value)
            {
                case GL_NEVER: return CompareOp::Never;
                case GL_LESS: return CompareOp::Less;
                case GL_EQUAL: return CompareOp::Equal;
                case GL_LEQUAL: return CompareOp::LessEqual;
                case GL_GREATER: return CompareOp::Greater;
                case GL_NOTEQUAL: return CompareOp::NotEqual;
                case GL_GEQUAL: return CompareOp::GreaterEqual;
                case GL_ALWAYS: return CompareOp::Always;
                default: return std::nullopt;
            }
        }

        [[nodiscard]] inline std::optional<RenderCore::BlendEquation> blendEquation(GLenum value) noexcept
        {
            using RenderCore::BlendEquation;
            switch (value)
            {
                case GL_FUNC_ADD: return BlendEquation::Add;
                case GL_FUNC_SUBTRACT: return BlendEquation::Subtract;
                case GL_FUNC_REVERSE_SUBTRACT: return BlendEquation::ReverseSubtract;
                case GL_MIN: return BlendEquation::Minimum;
                case GL_MAX: return BlendEquation::Maximum;
                default: return std::nullopt;
            }
        }

        [[nodiscard]] inline RenderCore::TextureRole textureRole(std::string_view name) noexcept
        {
            using RenderCore::TextureRole;
            if (name == "darkMap") return TextureRole::Dark;
            if (name == "detailMap") return TextureRole::Detail;
            if (name == "decalMap") return TextureRole::Decal;
            if (name == "emissiveMap") return TextureRole::Emissive;
            if (name == "normalMap") return TextureRole::Normal;
            if (name == "envMap") return TextureRole::Environment;
            if (name == "specularMap") return TextureRole::Specular;
            if (name == "bumpMap") return TextureRole::Bump;
            if (name == "glossMap") return TextureRole::Gloss;
            return TextureRole::Diffuse;
        }

        [[nodiscard]] inline RenderCore::TextureWrap textureWrap(osg::Texture::WrapMode value) noexcept
        {
            switch (value)
            {
                case osg::Texture::REPEAT: return RenderCore::TextureWrap::Repeat;
                case osg::Texture::MIRROR: return RenderCore::TextureWrap::Mirror;
                case osg::Texture::CLAMP_TO_BORDER: return RenderCore::TextureWrap::Border;
                case osg::Texture::CLAMP:
                case osg::Texture::CLAMP_TO_EDGE:
                default: return RenderCore::TextureWrap::Clamp;
            }
        }

        inline void textureFilter(const osg::Texture& texture, RenderCore::SamplerSemantic& sampler) noexcept
        {
            using Filter = osg::Texture::FilterMode;
            const Filter min = texture.getFilter(osg::Texture::MIN_FILTER);
            const Filter mag = texture.getFilter(osg::Texture::MAG_FILTER);
            sampler.magFilter = mag == Filter::NEAREST ? RenderCore::TextureFilter::Nearest
                                                       : RenderCore::TextureFilter::Linear;
            switch (min)
            {
                case Filter::NEAREST:
                    sampler.minFilter = RenderCore::TextureFilter::Nearest;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::None;
                    break;
                case Filter::LINEAR:
                    sampler.minFilter = RenderCore::TextureFilter::Linear;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::None;
                    break;
                case Filter::NEAREST_MIPMAP_NEAREST:
                    sampler.minFilter = RenderCore::TextureFilter::Nearest;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::Nearest;
                    break;
                case Filter::LINEAR_MIPMAP_NEAREST:
                    sampler.minFilter = RenderCore::TextureFilter::Linear;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::Nearest;
                    break;
                case Filter::NEAREST_MIPMAP_LINEAR:
                    sampler.minFilter = RenderCore::TextureFilter::Nearest;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::Linear;
                    break;
                case Filter::LINEAR_MIPMAP_LINEAR:
                default:
                    sampler.minFilter = RenderCore::TextureFilter::Linear;
                    sampler.mipmapMode = RenderCore::TextureMipmapMode::Linear;
                    break;
            }
            sampler.wrapU = textureWrap(texture.getWrap(osg::Texture::WRAP_S));
            sampler.wrapV = textureWrap(texture.getWrap(osg::Texture::WRAP_T));
            sampler.maxAnisotropy = texture.getMaxAnisotropy();
        }

        [[nodiscard]] inline osg::ref_ptr<osg::StateSet> effectiveState(
            const osg::NodePath& path, const osg::StateSet* drawable = nullptr)
        {
            osg::ref_ptr<osg::StateSet> result = new osg::StateSet;
            for (const osg::Node* node : path)
            {
                if (node && node->getStateSet())
                    result->merge(*node->getStateSet());
            }
            if (drawable)
                result->merge(*drawable);
            return result;
        }

        [[nodiscard]] inline bool stateEnabled(
            const osg::StateSet& state, GLenum mode, bool defaultValue = false) noexcept
        {
            const osg::StateAttribute::GLModeValue value = state.getMode(mode);
            if (value == osg::StateAttribute::INHERIT)
                return defaultValue;
            return (value & osg::StateAttribute::ON) != 0;
        }

        [[nodiscard]] inline RenderCore::TextureApplyMode textureApplyMode(const osg::NodePath& path) noexcept
        {
            int value = 2;
            for (const osg::Node* node : path)
            {
                int candidate = 0;
                if (node && node->getUserValue("applyMode", candidate))
                    value = candidate;
            }
            switch (value)
            {
                case 0: return RenderCore::TextureApplyMode::Replace;
                case 1: return RenderCore::TextureApplyMode::Decal;
                case 3: return RenderCore::TextureApplyMode::Highlight;
                case 4: return RenderCore::TextureApplyMode::Highlight2;
                case 2:
                default: return RenderCore::TextureApplyMode::Modulate;
            }
        }

        [[nodiscard]] inline bool noLightingShader(const osg::NodePath& path)
        {
            std::string shaderPrefix;
            for (const osg::Node* node : path)
            {
                std::string candidate;
                if (node && node->getUserValue("shaderPrefix", candidate))
                    shaderPrefix = std::move(candidate);
            }
            return shaderPrefix.find("nolighting") != std::string::npos;
        }

        inline void applyTexMat(const osg::StateSet& state, unsigned int unit, std::vector<glm::vec2>& coords)
        {
            const auto* texMat = dynamic_cast<const osg::TexMat*>(
                state.getTextureAttribute(unit, osg::StateAttribute::TEXMAT));
            if (!texMat)
                return;
            const osg::Matrix& matrix = texMat->getMatrix();
            for (glm::vec2& coord : coords)
            {
                const osg::Vec3f transformed = osg::Vec3f(coord.x, coord.y, 0.0f) * matrix;
                coord = { transformed.x(), transformed.y() };
            }
        }

        struct CapturedMaterial
        {
            RenderCore::MaterialRecord material;
            std::vector<RenderCore::EffectTextureSnapshot> textures;
        };

        [[nodiscard]] inline bool captureMaterial(const osg::NodePath& path, const osg::StateSet* drawableState,
            CapturedMaterial& out, std::string& diagnostic)
        {
            using namespace RenderCore;
            const osg::ref_ptr<osg::StateSet> state = effectiveState(path, drawableState);
            MaterialRecord material;
            material.textureApply = textureApplyMode(path);
            material.unlit = noLightingShader(path);

            if (const auto* source = dynamic_cast<const SceneUtil::Material*>(
                    state->getAttribute(osg::StateAttribute::MATERIAL)))
            {
                material.diffuse = toGlm(source->getDiffuse());
                material.ambient = toGlm(source->getAmbient());
                material.specular = toGlm(source->getSpecular());
                material.emission = toGlm(source->getEmission());
                material.shininess = source->getShininess();
                material.emissiveMultiplier = source->getEmissiveMultiplier();
                material.specularStrength = source->getSpecularStrength();
                switch (source->getVertexColorMode())
                {
                    case SceneUtil::VertexColorModes::Emission:
                        material.vertexColorMode = VertexColorMode::Emissive;
                        break;
                    case SceneUtil::VertexColorModes::AmbientAndDiffuse:
                    case SceneUtil::VertexColorModes::Ambient:
                    case SceneUtil::VertexColorModes::Diffuse:
                        material.vertexColorMode = VertexColorMode::AmbientDiffuse;
                        break;
                    case SceneUtil::VertexColorModes::None:
                    case SceneUtil::VertexColorModes::Specular:
                    default:
                        material.vertexColorMode = VertexColorMode::Ignore;
                        break;
                }
            }
            material.alpha = material.diffuse.a;
            if (const osg::Uniform* alpha = state->getUniform("alpha"))
            {
                float value = 1.0f;
                if (alpha->get(value))
                {
                    value = std::clamp(value, 0.0f, 1.0f);
                    material.alpha *= value;
                    material.diffuse.a *= value;
                    material.ambient.a *= value;
                    material.specular.a *= value;
                    material.emission.a *= value;
                }
            }

            material.alphaBlendEnabled = stateEnabled(*state, GL_BLEND, false);
            if (material.alphaBlendEnabled)
            {
                material.alphaMode = AlphaMode::Blend;
                material.transparentSort = state->getBinName() == "TraversalOrderBin"
                    ? TransparentSortPolicy::Unsorted
                    : TransparentSortPolicy::Sorted;
                if (const auto* blend = dynamic_cast<const osg::BlendFunc*>(
                        state->getAttribute(osg::StateAttribute::BLENDFUNC)))
                {
                    const auto source = blendFactor(blend->getSource());
                    const auto destination = blendFactor(blend->getDestination());
                    if (!source || !destination)
                    {
                        diagnostic = "evaluated effect uses an unsupported blend factor";
                        return false;
                    }
                    material.sourceBlend = *source;
                    material.destinationBlend = *destination;
                }
                if (const auto* equation = dynamic_cast<const osg::BlendEquation*>(
                        state->getAttribute(osg::StateAttribute::BLENDEQUATION)))
                {
                    const auto value = blendEquation(equation->getEquation());
                    if (!value)
                    {
                        diagnostic = "evaluated effect uses an unsupported blend equation";
                        return false;
                    }
                    material.blendEquation = *value;
                }
            }

            if (const auto* alpha = dynamic_cast<const osg::AlphaFunc*>(
                    state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
            {
                const auto compare = compareOp(alpha->getFunction());
                if (!compare)
                {
                    diagnostic = "evaluated effect uses an unsupported alpha comparison";
                    return false;
                }
                material.alphaTestEnabled = true;
                material.alphaCompare = *compare;
                material.alphaCutoff = alpha->getReferenceValue();
                if (!material.alphaBlendEnabled)
                    material.alphaMode = AlphaMode::Mask;
            }

            material.cullMode = CullMode::Back;
            if (!stateEnabled(*state, GL_CULL_FACE, true))
                material.cullMode = CullMode::None;
            else if (const auto* cull = dynamic_cast<const osg::CullFace*>(
                         state->getAttribute(osg::StateAttribute::CULLFACE)))
            {
                if (cull->getMode() == osg::CullFace::FRONT)
                    material.cullMode = CullMode::Front;
                else if (cull->getMode() == osg::CullFace::FRONT_AND_BACK)
                {
                    diagnostic = "evaluated effect culls both faces";
                    return false;
                }
            }

            material.depthTest = stateEnabled(*state, GL_DEPTH_TEST, true);
            if (const auto* depth = dynamic_cast<const osg::Depth*>(
                    state->getAttribute(osg::StateAttribute::DEPTH)))
            {
                material.depthWrite = depth->getWriteMask();
                if (depth->getFunction() == osg::Depth::ALWAYS)
                    material.depthTest = false;
            }

            if (const auto* polygon = dynamic_cast<const osg::PolygonMode*>(
                    state->getAttribute(osg::StateAttribute::POLYGONMODE)))
            {
                material.wireframe = polygon->getMode(osg::PolygonMode::FRONT_AND_BACK) == osg::PolygonMode::LINE;
            }

            if (const osg::Uniform* depth = state->getUniform("fog.depth"))
            {
                float value = 0.0f;
                if (depth->get(value))
                {
                    if (value < 0.0f || value >= 1000000.0f)
                        material.fog.mode = MaterialFogMode::Disabled;
                    else
                    {
                        material.fog.mode = MaterialFogMode::Override;
                        material.fog.depth = value;
                        if (const osg::Uniform* color = state->getUniform("fog.color"))
                        {
                            osg::Vec4f sourceColor;
                            if (color->get(sourceColor))
                                material.fog.color = toGlm(sourceColor);
                        }
                    }
                }
            }

            const auto& textureAttributes = state->getTextureAttributeList();
            for (unsigned int unit = 0; unit < textureAttributes.size(); ++unit)
            {
                const auto* texture = dynamic_cast<const osg::Texture2D*>(
                    state->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                if (!texture)
                    continue;
                const osg::Image* image = texture->getImage();
                if (!image || image->getFileName().empty() || image->s() <= 0 || image->t() <= 0)
                {
                    diagnostic = "evaluated effect texture has no recoverable winning-VFS image identity";
                    return false;
                }

                std::string typeName;
                if (const auto* type = dynamic_cast<const SceneUtil::TextureType*>(
                        state->getTextureAttribute(unit, SceneUtil::TextureType::AttributeType)))
                    typeName = type->getName();
                const TextureRole role = textureRole(typeName);

                EffectTextureSnapshot snapshot;
                snapshot.texture.revision = InitialResourceRevision;
                snapshot.texture.sourceIdentity = image->getFileName();
                snapshot.texture.contentIdentity = image->getFileName();
                snapshot.texture.width = static_cast<std::uint32_t>(image->s());
                snapshot.texture.height = static_cast<std::uint32_t>(image->t());
                snapshot.texture.mipmapped = image->getNumMipmapLevels() > 1;
                snapshot.binding.role = role;
                snapshot.binding.transform.uvSet = unit;
                snapshot.binding.colorSpace = role == TextureRole::Normal || role == TextureRole::Bump
                    || role == TextureRole::Gloss || role == TextureRole::Blend
                    ? TextureColorSpace::Data
                    : TextureColorSpace::Srgb;
                snapshot.binding.formatClass = role == TextureRole::Normal ? TextureFormatClass::Normal
                    : (role == TextureRole::Bump ? TextureFormatClass::Height : TextureFormatClass::Color);
                textureFilter(*texture, snapshot.binding.sampler);
                out.textures.push_back(std::move(snapshot));
            }

            out.material = std::move(material);
            return true;
        }

        [[nodiscard]] inline RenderCore::AxisAlignedBounds boundsFor(const std::vector<glm::vec3>& positions)
        {
            RenderCore::AxisAlignedBounds result;
            result.minimum = glm::vec3(std::numeric_limits<float>::max());
            result.maximum = glm::vec3(std::numeric_limits<float>::lowest());
            for (const glm::vec3& position : positions)
            {
                result.minimum = glm::min(result.minimum, position);
                result.maximum = glm::max(result.maximum, position);
            }
            return result;
        }

        [[nodiscard]] inline bool appendPrimitive(const osg::PrimitiveSet& primitive,
            RenderCore::MeshPayload& mesh, std::string& diagnostic)
        {
            const GLenum mode = primitive.getMode();
            const std::uint32_t first = static_cast<std::uint32_t>(mesh.indices.size());
            auto appendIndex = [&](unsigned int source) {
                mesh.indices.push_back(static_cast<std::uint32_t>(primitive.index(source)));
            };

            RenderCore::PrimitiveTopology topology = RenderCore::PrimitiveTopology::Triangles;
            if (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_LINES || mode == GL_POINTS)
            {
                if (mode == GL_TRIANGLE_STRIP) topology = RenderCore::PrimitiveTopology::TriangleStrip;
                else if (mode == GL_LINES) topology = RenderCore::PrimitiveTopology::Lines;
                else if (mode == GL_POINTS) topology = RenderCore::PrimitiveTopology::Points;
                for (unsigned int i = 0; i < primitive.getNumIndices(); ++i)
                    appendIndex(i);
            }
            else if (mode == GL_QUADS)
            {
                for (unsigned int i = 0; i + 3 < primitive.getNumIndices(); i += 4)
                {
                    const std::uint32_t a = primitive.index(i);
                    const std::uint32_t b = primitive.index(i + 1);
                    const std::uint32_t c = primitive.index(i + 2);
                    const std::uint32_t d = primitive.index(i + 3);
                    mesh.indices.insert(mesh.indices.end(), { a, b, c, c, d, a });
                }
            }
            else if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON)
            {
                if (primitive.getNumIndices() >= 3)
                {
                    const std::uint32_t firstVertex = primitive.index(0);
                    for (unsigned int i = 1; i + 1 < primitive.getNumIndices(); ++i)
                        mesh.indices.insert(mesh.indices.end(), { firstVertex,
                            static_cast<std::uint32_t>(primitive.index(i)),
                            static_cast<std::uint32_t>(primitive.index(i + 1)) });
                }
            }
            else if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP)
            {
                topology = RenderCore::PrimitiveTopology::Lines;
                for (unsigned int i = 0; i + 1 < primitive.getNumIndices(); ++i)
                    mesh.indices.insert(mesh.indices.end(), { static_cast<std::uint32_t>(primitive.index(i)),
                        static_cast<std::uint32_t>(primitive.index(i + 1)) });
                if (mode == GL_LINE_LOOP && primitive.getNumIndices() > 2)
                    mesh.indices.insert(mesh.indices.end(), { static_cast<std::uint32_t>(primitive.index(primitive.getNumIndices() - 1)),
                        static_cast<std::uint32_t>(primitive.index(0)) });
            }
            else
            {
                diagnostic = "evaluated effect uses an unsupported primitive topology";
                return false;
            }

            const std::uint32_t count = static_cast<std::uint32_t>(mesh.indices.size()) - first;
            if (count != 0)
                mesh.surfaces.push_back({ topology, first, count, 0u });
            return true;
        }

        [[nodiscard]] inline bool captureGeometry(const osg::Geometry& geometry, const osg::NodePath& path,
            std::string identity, RenderCore::ImmediateEffectDraw& draw, std::string& diagnostic)
        {
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (!positions || positions->empty())
            {
                diagnostic = "evaluated effect geometry has no supported position stream";
                return false;
            }
            draw.identity = std::move(identity);
            draw.worldTransform = toGlm(osg::computeLocalToWorld(path));
            if (!finite(draw.worldTransform))
            {
                diagnostic = "evaluated effect geometry has a non-finite world transform";
                return false;
            }
            draw.mesh.positions.reserve(positions->size());
            for (const osg::Vec3f& position : *positions)
                draw.mesh.positions.push_back(toGlm(position));

            if (const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray()))
            {
                if (normals->size() == positions->size())
                {
                    draw.mesh.normals.reserve(normals->size());
                    for (const osg::Vec3f& normal : *normals)
                        draw.mesh.normals.push_back(toGlm(normal));
                }
            }

            if (const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry.getColorArray()))
            {
                if (colors->size() == positions->size())
                {
                    draw.mesh.colors.reserve(colors->size());
                    for (const osg::Vec4f& color : *colors)
                        draw.mesh.colors.push_back(toGlm(color));
                }
            }
            else if (const auto* colors = dynamic_cast<const osg::Vec4ubArray*>(geometry.getColorArray()))
            {
                if (colors->size() == positions->size())
                {
                    draw.mesh.colors.reserve(colors->size());
                    for (const osg::Vec4ub& color : *colors)
                        draw.mesh.colors.emplace_back(color.r() / 255.0f, color.g() / 255.0f,
                            color.b() / 255.0f, color.a() / 255.0f);
                }
            }

            CapturedMaterial captured;
            if (!captureMaterial(path, geometry.getStateSet(), captured, diagnostic))
                return false;
            draw.material = std::move(captured.material);
            draw.textures = std::move(captured.textures);

            const std::size_t textureSets = draw.textures.empty() ? 0u
                : static_cast<std::size_t>(std::max_element(draw.textures.begin(), draw.textures.end(),
                      [](const auto& left, const auto& right) {
                          return left.binding.transform.uvSet < right.binding.transform.uvSet;
                      })->binding.transform.uvSet + 1u);
            draw.mesh.texCoordSets.resize(textureSets);
            const osg::ref_ptr<osg::StateSet> state = effectiveState(path, geometry.getStateSet());
            for (std::size_t set = 0; set < textureSets; ++set)
            {
                const auto* coords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(set));
                if (!coords || coords->size() != positions->size())
                {
                    diagnostic = "evaluated effect textured geometry has no matching UV stream";
                    return false;
                }
                auto& destination = draw.mesh.texCoordSets[set];
                destination.reserve(coords->size());
                for (const osg::Vec2f& coord : *coords)
                    destination.emplace_back(coord.x(), coord.y());
                applyTexMat(*state, static_cast<unsigned int>(set), destination);
            }

            for (unsigned int i = 0; i < geometry.getNumPrimitiveSets(); ++i)
            {
                const osg::PrimitiveSet* primitive = geometry.getPrimitiveSet(i);
                if (primitive && !appendPrimitive(*primitive, draw.mesh, diagnostic))
                    return false;
            }
            if (draw.mesh.surfaces.empty())
            {
                diagnostic = "evaluated effect geometry has no supported draw primitives";
                return false;
            }
            draw.bounds = boundsFor(draw.mesh.positions);
            return RenderCore::validImmediateEffectDraw(draw);
        }

        [[nodiscard]] inline bool captureParticleSystem(const osgParticle::ParticleSystem& particles,
            const osg::NodePath& path, std::string_view identityPrefix,
            std::vector<RenderCore::ImmediateEffectDraw>& draws, std::string& diagnostic)
        {
            if (particles.getUseShaders())
            {
                diagnostic = "shader-evaluated particle system requires a CPU-equivalent V4 effect facet";
                return false;
            }
            if (particles.getSortMode() == osgParticle::ParticleSystem::SORT_FRONT_TO_BACK)
            {
                diagnostic = "front-to-back particle sorting is outside the evaluated V4 effect contract";
                return false;
            }
            if (particles.getVisibilityDistance() > 0.0)
            {
                diagnostic = "particle visibility-distance culling requires an explicit V4 effect facet";
                return false;
            }
            CapturedMaterial captured;
            if (!captureMaterial(path, particles.getStateSet(), captured, diagnostic))
                return false;
            if (particles.getSortMode() == osgParticle::ParticleSystem::NO_SORT)
                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;
            else if (captured.material.alphaBlendEnabled)
                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Sorted;
            const osg::ref_ptr<osg::StateSet> state = effectiveState(path, particles.getStateSet());
            const glm::mat4 systemWorld = toGlm(osg::computeLocalToWorld(path));
            if (!finite(systemWorld))
            {
                diagnostic = "evaluated particle system has a non-finite world transform";
                return false;
            }

            const int detail = std::max(1, particles.getLevelOfDetail());
            const float detailScale = std::sqrt(static_cast<float>(detail));
            std::size_t ordinal = 0;
            for (int i = 0; i < particles.numParticles(); i += detail)
            {
                const osgParticle::Particle* particle = particles.getParticle(i);
                if (!particle || !particle->isAlive())
                    continue;
                RenderCore::ImmediateEffectDraw draw;
                draw.identity = std::string(identityPrefix) + ":particle:" + std::to_string(ordinal++);
                draw.material = captured.material;
                draw.textures = captured.textures;

                const osg::Vec3f position = particle->getPosition();
                draw.worldTransform = systemWorld * glm::translate(glm::mat4(1.0f), toGlm(position));

                const osg::Vec4f sourceColor = particle->getCurrentColor();
                const glm::vec4 color{ sourceColor.r(), sourceColor.g(), sourceColor.b(),
                    sourceColor.a() * particle->getCurrentAlpha() };
                const float size = particle->getCurrentSize() * detailScale;
                const osg::Vec3f angle = particle->getAngle();
                osg::Matrixf rotation;
                rotation.makeRotate(angle.x(), osg::Vec3f(1.0f, 0.0f, 0.0f), angle.y(), osg::Vec3f(0.0f, 1.0f, 0.0f),
                    angle.z(), osg::Vec3f(0.0f, 0.0f, 1.0f));
                osg::Vec3f xAxis = osg::Matrixf::transform3x3(particles.getAlignVectorX(), rotation) * size;
                osg::Vec3f yAxis = osg::Matrixf::transform3x3(particles.getAlignVectorY(), rotation) * size;

                const auto addTextureCoords = [&](std::vector<glm::vec2>& target) {
                    const float s = particle->getSTexCoord();
                    const float t = particle->getTTexCoord();
                    const float sw = particle->getSTexTile();
                    const float th = particle->getTTexTile();
                    target = { { s, t }, { s + sw, t }, { s + sw, t + th }, { s, t + th } };
                };

                switch (particle->getShape())
                {
                    case osgParticle::Particle::POINT:
                        draw.mesh.positions.emplace_back(0.0f);
                        draw.mesh.colors.push_back(color);
                        draw.mesh.indices.push_back(0u);
                        draw.mesh.surfaces.push_back({ RenderCore::PrimitiveTopology::Points, 0u, 1u, 0u });
                        break;
                    case osgParticle::Particle::LINE:
                        diagnostic = "evaluated line particles require the directional-line effect facet";
                        return false;
                    case osgParticle::Particle::USER:
                    case osgParticle::Particle::QUAD_TRIANGLESTRIP:
                    case osgParticle::Particle::HEXAGON:
                    case osgParticle::Particle::QUAD:
                    default:
                    {
                        draw.mesh.positions = { -toGlm(xAxis) - toGlm(yAxis), toGlm(xAxis) - toGlm(yAxis),
                            toGlm(xAxis) + toGlm(yAxis), -toGlm(xAxis) + toGlm(yAxis) };
                        draw.mesh.normals.assign(4, glm::vec3(0.0f, 0.0f, 1.0f));
                        draw.mesh.colors.assign(4, color);
                        draw.mesh.indices = { 0u, 1u, 2u, 2u, 3u, 0u };
                        draw.mesh.surfaces.push_back({ RenderCore::PrimitiveTopology::Triangles, 0u, 6u, 0u });
                        if (particles.getParticleAlignment() == osgParticle::ParticleSystem::BILLBOARD)
                            draw.billboard = RenderCore::ModelBillboardMode::AlwaysFaceCamera;
                        break;
                    }
                }

                const std::size_t textureSets = draw.textures.empty() ? 0u
                    : static_cast<std::size_t>(std::max_element(draw.textures.begin(), draw.textures.end(),
                          [](const auto& left, const auto& right) {
                              return left.binding.transform.uvSet < right.binding.transform.uvSet;
                          })->binding.transform.uvSet + 1u);
                draw.mesh.texCoordSets.resize(textureSets);
                for (std::size_t set = 0; set < textureSets; ++set)
                {
                    if (draw.mesh.positions.size() == 4)
                    {
                        addTextureCoords(draw.mesh.texCoordSets[set]);
                        applyTexMat(*state, static_cast<unsigned int>(set), draw.mesh.texCoordSets[set]);
                    }
                    else
                        draw.mesh.texCoordSets[set].assign(draw.mesh.positions.size(), glm::vec2(0.5f));
                }
                draw.bounds = boundsFor(draw.mesh.positions);
                if (!RenderCore::validImmediateEffectDraw(draw))
                {
                    diagnostic = "evaluated particle draw failed neutral validation";
                    return false;
                }
                draws.push_back(std::move(draw));
            }
            return true;
        }

        [[nodiscard]] inline bool isEffectRoot(const osg::Node& node) noexcept
        {
            for (const osg::Callback* callback = node.getUpdateCallback(); callback;
                 callback = callback->getNestedCallback())
            {
                if (dynamic_cast<const UpdateVfxCallback*>(callback))
                    return true;
            }
            return false;
        }

        class CaptureVisitor final : public osg::NodeVisitor
        {
        public:
            CaptureVisitor(std::string identityPrefix, bool wholeSubtree)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mIdentityPrefix(std::move(identityPrefix))
                , mWholeSubtree(wholeSubtree)
                , mDepth(wholeSubtree ? 1u : 0u)
            {
            }

            void apply(osg::Node& node) override
            {
                const bool entered = !mWholeSubtree && isEffectRoot(node);
                if (entered)
                    ++mDepth;
                if (mDepth != 0)
                {
                    if (const auto* particles = dynamic_cast<const osgParticle::ParticleSystem*>(&node))
                    {
                        if (!captureParticleSystem(*particles, getNodePath(), nextIdentity("system"), mResult.draws,
                                mResult.diagnostic))
                            return;
                    }
                }
                if (mResult.valid())
                    traverse(node);
                if (entered)
                    --mDepth;
            }

            void apply(osg::Geode& geode) override
            {
                const bool entered = !mWholeSubtree && isEffectRoot(geode);
                if (entered)
                    ++mDepth;
                if (mDepth != 0)
                {
                    for (unsigned int i = 0; i < geode.getNumDrawables() && mResult.valid(); ++i)
                    {
                        osg::Drawable* drawable = geode.getDrawable(i);
                        if (auto* geometry = dynamic_cast<osg::Geometry*>(drawable))
                        {
                            RenderCore::ImmediateEffectDraw draw;
                            if (!captureGeometry(*geometry, getNodePath(), nextIdentity("geometry"), draw,
                                    mResult.diagnostic))
                                break;
                            mResult.draws.push_back(std::move(draw));
                        }
                        else if (auto* particles = dynamic_cast<osgParticle::ParticleSystem*>(drawable))
                        {
                            if (!captureParticleSystem(*particles, getNodePath(), nextIdentity("system"),
                                    mResult.draws, mResult.diagnostic))
                                break;
                        }
                    }
                }
                if (mResult.valid())
                    traverse(geode);
                if (entered)
                    --mDepth;
            }

            [[nodiscard]] V4EffectCaptureResult take() { return std::move(mResult); }

        private:
            [[nodiscard]] std::string nextIdentity(std::string_view kind)
            {
                return mIdentityPrefix + ":" + std::string(kind) + ":" + std::to_string(mOrdinal++);
            }

            std::string mIdentityPrefix;
            bool mWholeSubtree = false;
            std::size_t mDepth = 0;
            std::size_t mOrdinal = 0;
            V4EffectCaptureResult mResult;
        };
    }

    [[nodiscard]] inline V4EffectCaptureResult captureV4AttachedEffects(
        osg::Node& animationRoot, std::string identityPrefix)
    {
        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), false);
        animationRoot.accept(visitor);
        return visitor.take();
    }

    [[nodiscard]] inline V4EffectCaptureResult captureV4WholeEffectSubtree(
        osg::Node& root, std::string identityPrefix)
    {
        v4_effect_detail::CaptureVisitor visitor(std::move(identityPrefix), true);
        root.accept(visitor);
        return visitor.take();
    }
}

#endif
