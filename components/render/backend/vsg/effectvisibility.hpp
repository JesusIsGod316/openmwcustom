#ifndef OPENMW_RENDER_VSG_EFFECTVISIBILITY_H
#define OPENMW_RENDER_VSG_EFFECTVISIBILITY_H
#include <components/rendercore/effectframe.hpp>
#include <vsg/nodes/CullGroup.h>
#include <cmath>

namespace RenderVsg
{
    // Local-space sphere below the world-placement transform. VSG tests it
    // against EACH recording view, not a main-camera visibility bit reused by
    // reflections or shadows. Compile traversal never uses this rejection.
    inline std::optional<vsg::dsphere> effectCullBound(const RenderCore::ImmediateEffectDraw& draw)
    {
        const auto& material = draw.material;
        if (!material.depthTest || material.stencil.enabled || material.treeAnimation || material.refraction
            || material.softEffect || material.wireframe || draw.meshData().positions.empty()) return {};
        for (const auto& surface : draw.meshData().surfaces)
            if (surface.topology == RenderCore::PrimitiveTopology::Lines
                || surface.topology == RenderCore::PrimitiveTopology::Points) return {}; // screen-space width
        RenderCore::AxisAlignedBounds bounds;
        if (draw.meshSnapshot)
        {
            if (!draw.meshSnapshot->valid()) return {};
            bounds = draw.meshSnapshot->bounds();
        }
        else
        {
            bounds.minimum = bounds.maximum = draw.meshData().positions.front();
            for (const auto& p : draw.meshData().positions)
            {
                if (!RenderCore::semantic_detail::finite(p)) return {};
                bounds.minimum = glm::min(bounds.minimum, p);
                bounds.maximum = glm::max(bounds.maximum, p);
            }
        }
        glm::dvec3 center = (glm::dvec3(bounds.minimum) + glm::dvec3(bounds.maximum)) * 0.5;
        double radius = glm::length(glm::dvec3(bounds.maximum) - center);
        if (draw.billboard)
        {
            // All authored billboard modes rotate around the local origin.
            center = glm::dvec3(0.0);
            radius = glm::length(glm::max(glm::abs(glm::dvec3(bounds.minimum)), glm::abs(glm::dvec3(bounds.maximum))));
        }
        // Numerical cushion; unknown/unbounded shader displacement stays open.
        radius += std::max(1e-4, radius * 1e-5);
        return vsg::dsphere(center.x, center.y, center.z, radius);
    }
}
#endif
