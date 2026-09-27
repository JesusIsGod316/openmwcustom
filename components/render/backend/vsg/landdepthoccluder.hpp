#ifndef OPENMW_RENDER_VSG_LANDDEPTHOCCLUDER_H
#define OPENMW_RENDER_VSG_LANDDEPTHOCCLUDER_H
#include <components/rendercore/records.hpp>

namespace RenderVsg
{
    inline bool landDepthOccluder(const RenderCore::MaterialRecord& material, bool weightedBase)
    {
        // LAND blend weights affect color, not coverage: the first layer writes
        // depth even where its texture weight is zero. Restrict this exception
        // to the native terrain base equation, not arbitrary translucent assets.
        const bool baseBlend = weightedBase && material.terrainLayer && material.terrainLayer->first
            && material.sourceBlend == RenderCore::BlendFactor::SourceAlpha
            && material.destinationBlend == RenderCore::BlendFactor::Zero
            && material.blendEquation == RenderCore::BlendEquation::Add && !material.separateAlphaBlend;
        return material.terrainLayer && (!material.alphaBlendEnabled || baseBlend)
            && !material.alphaTestEnabled && material.alpha == 1.f
            && material.depthTest && material.depthWrite && !material.wireframe
            && !material.decal && !material.stencil.enabled && !material.treeAnimation
            && !material.refraction && !material.softEffect;
    }
}
#endif
