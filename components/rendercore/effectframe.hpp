#ifndef OPENMW_COMPONENTS_RENDERCORE_EFFECTFRAME_H
#define OPENMW_COMPONENTS_RENDERCORE_EFFECTFRAME_H

#include "records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace RenderCore
{
    // Low-volume evaluated effect payload copied from OpenMW's authoritative
    // gameplay-side animation/particle evaluator at the frame boundary. The
    // backend receives ordinary neutral mesh/material semantics; no OSG node,
    // callback, particle object, or mutable controller state crosses this seam.
    struct EffectTextureSnapshot
    {
        TextureRecord texture;
        // The handle is intentionally invalid in a frame snapshot. A backend
        // that materializes the transient draw owns the temporary TextureHandle
        // and substitutes it before normal material realization.
        TextureBinding binding;
    };

    struct ImmediateEffectDraw
    {
        std::string identity;
        glm::mat4 worldTransform{ 1.0f };
        MeshPayload mesh;
        AxisAlignedBounds bounds;
        // Frame snapshots carry material values directly. Persistent texture
        // handles are not valid here; textures must be described by `textures`.
        MaterialRecord material;
        std::vector<EffectTextureSnapshot> textures;
        std::uint64_t semanticFlags = semanticFlag(InstanceSemanticFlag::Effect)
            | semanticFlag(InstanceSemanticFlag::ReflectionEligible)
            | semanticFlag(InstanceSemanticFlag::RefractionEligible);
    };

    [[nodiscard]] inline bool validImmediateEffectDraw(const ImmediateEffectDraw& draw) noexcept
    {
        if (draw.identity.empty() || !semantic_detail::finite(draw.worldTransform) || !validMeshPayload(draw.mesh)
            || draw.mesh.positions.empty() || draw.mesh.surfaces.empty() || !draw.material.textures.empty())
            return false;
        if (!semantic_detail::finite(draw.bounds.minimum) || !semantic_detail::finite(draw.bounds.maximum)
            || draw.bounds.minimum.x > draw.bounds.maximum.x || draw.bounds.minimum.y > draw.bounds.maximum.y
            || draw.bounds.minimum.z > draw.bounds.maximum.z)
            return false;
        for (const MeshSurface& surface : draw.mesh.surfaces)
        {
            if (surface.materialSlot != 0)
                return false;
        }
        for (const EffectTextureSnapshot& texture : draw.textures)
        {
            if (texture.binding.texture.valid() || texture.texture.sourceIdentity.empty()
                || texture.texture.contentIdentity.empty() || texture.texture.width == 0 || texture.texture.height == 0
                || !texture.texture.revision.valid())
                return false;
        }
        return true;
    }
}

#endif
