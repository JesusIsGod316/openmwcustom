#ifndef OPENMW_RENDER_VSG_EFFECTIDLEBUDGET_H
#define OPENMW_RENDER_VSG_EFFECTIDLEBUDGET_H

#include <components/rendercore/effectframe.hpp>

namespace RenderVsg
{
    // Admission is deliberately limited to small, conventional particle quads.
    // Charge texture storage even when shared with active residents, include a
    // conservative descriptor/graph allowance, and reject oversized/unknown
    // resources. This is a retention estimate, not a device allocator/RSS claim.
    inline std::size_t smallEffectIdleCost(const RenderCore::ImmediateEffectDraw& effect)
    {
        const auto& mesh = effect.meshData();
        if (mesh.positions.size() != 4 || mesh.indices.size() != 6 || mesh.surfaces.size() != 1
            || mesh.surfaces.front().topology != RenderCore::PrimitiveTopology::Triangles
            || mesh.texCoordSets.size() > 2 || effect.textures.size() > 2) return 0;
        std::size_t bytes = 64 * 1024;
        for (const auto& snapshot : effect.textures)
        {
            const auto& image = snapshot.texture;
            if (!image.width || !image.height || image.width > 1024 || image.height > 1024) return 0;
            // Up to RGBA32F plus a full mip chain; procedural CPU pixels are
            // counted separately. Capped dimensions make arithmetic bounded.
            bytes += std::size_t(image.width) * image.height * 16 * 2;
            if (image.pixels)
            {
                if (image.pixels->rgba8.size() > 4 * 1024 * 1024) return 0;
                bytes += image.pixels->rgba8.size();
            }
        }
        return bytes;
    }
}
#endif
