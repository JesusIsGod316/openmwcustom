#ifndef OPENMW_RENDERCORE_SKYFRAME_H
#define OPENMW_RENDERCORE_SKYFRAME_H

#include "effectframe.hpp"
#include <memory>

namespace RenderCore
{
    // Values match the canonical OpenMW sky passes, not a replacement weather
    // model. Geometry and winning texture identity are immutable; the small
    // transform/colour/opacity block is evaluated by the gameplay sky controller.
    enum class SkyPass : std::uint8_t { Atmosphere, Night, Clouds, Moon, Sun };

    struct SkyDrawSnapshot
    {
        std::string identity;
        SkyPass pass = SkyPass::Atmosphere;
        std::shared_ptr<const MeshPayload> mesh;
        glm::mat4 transform{1.f};
        glm::mat4 uvTransform{1.f};
        glm::vec4 diffuseColor{1.f};
        glm::vec4 moonBlend{0.f};
        glm::vec4 atmosphereFade{0.f};
        float opacity = 1.f;
        bool blendEnabled = true;
        BlendFactor sourceBlend = BlendFactor::SourceAlpha;
        BlendFactor destinationBlend = BlendFactor::OneMinusSourceAlpha;
        CullMode cullMode = CullMode::Back;
        FrontFaceWinding frontFace = FrontFaceWinding::CounterClockwise;
        std::vector<EffectTextureSnapshot> textures;
    };

    struct NativeSkySnapshot
    {
        std::vector<SkyDrawSnapshot> draws;
        std::uint32_t deferredOcclusionDraws = 0;
    };

    [[nodiscard]] inline bool validSkyDraw(const SkyDrawSnapshot& draw) noexcept
    {
        if (draw.identity.empty() || !draw.mesh || !validMeshPayload(*draw.mesh)
            || draw.mesh->positions.empty() || draw.mesh->surfaces.empty()
            || draw.mesh->colors.size() != draw.mesh->positions.size()
            || draw.mesh->texCoordSets.size() != 1
            || !semantic_detail::finite(draw.transform) || !semantic_detail::finite(draw.uvTransform)
            || !semantic_detail::finite(draw.diffuseColor) || !semantic_detail::finite(draw.moonBlend)
            || !semantic_detail::finite(draw.atmosphereFade) || !std::isfinite(draw.opacity)
            || static_cast<unsigned int>(draw.pass) > static_cast<unsigned int>(SkyPass::Sun))
            return false;
        const std::size_t expected = draw.pass == SkyPass::Atmosphere ? 0 : draw.pass == SkyPass::Moon ? 2 : 1;
        if (draw.textures.size() != expected) return false;
        for (const auto& image : draw.textures)
            if (image.texture.sourceIdentity.empty() || image.texture.contentIdentity.empty()
                || !image.texture.revision.valid() || image.texture.width == 0 || image.texture.height == 0
                || image.binding.texture.valid()) return false;
        for (const auto& surface : draw.mesh->surfaces)
            if (surface.topology != PrimitiveTopology::Triangles && surface.topology != PrimitiveTopology::TriangleStrip)
                return false;
        return true;
    }
    [[nodiscard]] inline bool validNativeSky(const NativeSkySnapshot& sky) noexcept
    {
        for (std::size_t i = 0; i < sky.draws.size(); ++i)
        {
            if (!validSkyDraw(sky.draws[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (sky.draws[i].identity == sky.draws[j].identity) return false;
        }
        return true;
    }
}
#endif
