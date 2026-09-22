#ifndef OPENMW_APPS_OPENMW_MWRENDER_V4EFFECTUV_H
#define OPENMW_APPS_OPENMW_MWRENDER_V4EFFECTUV_H

#include <components/rendercore/effectframe.hpp>

#include <osg/Geometry>
#include <osg/StateSet>
#include <osg/TexMat>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace MWRender::v4_effect_detail
{
    // A texture unit is an OSG state slot, not a dense mesh-UV index. Capture
    // only bound stages, retaining each stage's own TexMat even when several
    // stages share the same source array. Missing explicit arrays follow native
    // ShaderVisitor::adjustGeometry: unit zero, or the first non-null array when
    // zero is absent. Malformed arrays are never replaced by a different one.
    [[nodiscard]] inline bool captureEffectTextureCoordinates(const osg::Geometry& geometry,
        const osg::StateSet& state, RenderCore::ImmediateEffectDraw& draw, std::string& diagnostic)
    {
        const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (!positions || positions->empty())
        {
            diagnostic = "evaluated effect UV capture requires nonempty Vec3 positions";
            return false;
        }
        if (draw.textures.size() > std::numeric_limits<std::uint32_t>::max())
        {
            diagnostic = "evaluated effect has too many texture-coordinate bindings";
            return false;
        }

        std::vector<std::vector<glm::vec2>> captured;
        captured.reserve(draw.textures.size());
        for (const auto& texture : draw.textures)
        {
            const unsigned int unit = texture.binding.transform.uvSet;
            unsigned int sourceSet = unit;
            const osg::Array* source = geometry.getTexCoordArray(unit);
            const bool generated = (state.getTextureMode(unit, GL_TEXTURE_GEN_S) & osg::StateAttribute::ON)
                || (state.getTextureMode(unit, GL_TEXTURE_GEN_T) & osg::StateAttribute::ON)
                || (state.getTextureMode(unit, GL_TEXTURE_GEN_R) & osg::StateAttribute::ON)
                || (state.getTextureMode(unit, GL_TEXTURE_GEN_Q) & osg::StateAttribute::ON);
            const auto fail = [&](const std::string& reason) {
                diagnostic = "evaluated effect textured geometry has no matching UV stream: " + reason
                    + " [drawable='" + geometry.getName() + "', type=" + geometry.className()
                    + ", texture='" + texture.texture.sourceIdentity + "', unit=" + std::to_string(unit)
                    + ", source_set=" + std::to_string(sourceSet)
                    + ", array=" + (source ? std::string(source->className()) : std::string("none"))
                    + ", elements=" + std::to_string(source ? source->getNumElements() : 0u)
                    + ", vertices=" + std::to_string(positions->size())
                    + ", texgen=" + (generated ? "enabled" : "disabled") + "]";
                return false;
            };
            if (!source)
            {
                // Generated coordinates need a dedicated semantic, not borrowed
                // mesh coordinates. Preserve fail-closed behavior for this case.
                if (generated)
                    return fail("generated-coordinate stage requires semantic capture");
                sourceSet = 0;
                source = geometry.getTexCoordArray(0);
                if (!source)
                {
                    const auto& arrays = geometry.getTexCoordArrayList();
                    for (unsigned int candidate = 0; candidate < arrays.size(); ++candidate)
                    {
                        if (arrays[candidate])
                        {
                            sourceSet = candidate;
                            source = arrays[candidate].get();
                            break;
                        }
                    }
                }
            }
            const auto* coordinates = dynamic_cast<const osg::Vec2Array*>(source);
            if (!coordinates || coordinates->size() != positions->size())
                return fail("no valid explicit Vec2 stream after native array selection");

            auto& destination = captured.emplace_back();
            destination.reserve(coordinates->size());
            const auto* matrix = dynamic_cast<const osg::TexMat*>(
                state.getTextureAttribute(unit, osg::StateAttribute::TEXMAT));
            for (const osg::Vec2& uv : *coordinates)
            {
                if (matrix)
                {
                    const osg::Vec3 transformed = osg::Vec3(uv.x(), uv.y(), 0.0f) * matrix->getMatrix();
                    destination.emplace_back(transformed.x(), transformed.y());
                }
                else
                    destination.emplace_back(uv.x(), uv.y());
            }
        }

        // Commit only after every stage succeeds. A rejected later binding must
        // not leave earlier bindings remapped against an incomplete mesh.
        draw.mesh.texCoordSets = std::move(captured);
        for (std::size_t index = 0; index < draw.textures.size(); ++index)
            draw.textures[index].binding.transform.uvSet = static_cast<std::uint32_t>(index);
        return true;
    }
}

#endif
