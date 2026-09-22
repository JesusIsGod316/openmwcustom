#ifndef OPENMW_RENDER_VSG_IMMEDIATEEFFECTCONTRACT_H
#define OPENMW_RENDER_VSG_IMMEDIATEEFFECTCONTRACT_H
#include <components/rendercore/effectframe.hpp>
#include <cstddef>
#include <iterator>
namespace RenderVsg
{
    enum class ImmediateEffectMismatch : std::size_t
    {
        None, Identity, Billboard, BoundsControl, Material, Semantics, Indices,
        TangentBasis, VertexLayout, UvLayout, Surfaces, TextureCount,
        TextureIdentity, TextureBinding, StreamUpdate, NoCompletedResident, Count
    };
    inline const char* immediateEffectMismatchName(ImmediateEffectMismatch reason) noexcept
    {
        constexpr const char* names[] = { "none", "identity", "billboard", "bounds_control", "material",
            "semantics", "indices", "tangent_basis", "vertex_layout", "uv_layout", "surfaces",
            "texture_count", "texture_identity", "texture_binding", "stream_update", "no_completed_resident" };
        const auto index = static_cast<std::size_t>(reason);
        return index < std::size(names) ? names[index] : "invalid";
    }
    [[nodiscard]] inline ImmediateEffectMismatch immediateEffectLayoutMismatch(
        const RenderCore::ImmediateEffectDraw& resident, const RenderCore::ImmediateEffectDraw& current,
        bool immutableBoundsControl = false) noexcept
    {
        using Reason = ImmediateEffectMismatch;
        // Same reuse contract as before; now return the first failing predicate.
        if (resident.identity != current.identity) return Reason::Identity;
        if (resident.billboard != current.billboard) return Reason::Billboard;
        if (immutableBoundsControl && (resident.bounds.minimum != current.bounds.minimum
            || resident.bounds.maximum != current.bounds.maximum)) return Reason::BoundsControl;
        if (resident.material != current.material) return Reason::Material;
        if (resident.semanticFlags != current.semanticFlags) return Reason::Semantics;
        if (resident.mesh.indices != current.mesh.indices) return Reason::Indices;
        if (resident.mesh.tangents != current.mesh.tangents || resident.mesh.bitangents != current.mesh.bitangents)
            return Reason::TangentBasis;
        if (resident.mesh.positions.size() != current.mesh.positions.size()
            || resident.mesh.normals.size() != current.mesh.normals.size()
            || resident.mesh.colors.size() != current.mesh.colors.size()) return Reason::VertexLayout;
        if (resident.mesh.texCoordSets.size() != current.mesh.texCoordSets.size()) return Reason::UvLayout;
        if (resident.mesh.surfaces.size() != current.mesh.surfaces.size()) return Reason::Surfaces;
        if (resident.textures.size() != current.textures.size()) return Reason::TextureCount;
        for (std::size_t i = 0; i < resident.mesh.texCoordSets.size(); ++i)
            if (resident.mesh.texCoordSets[i].size() != current.mesh.texCoordSets[i].size()) return Reason::UvLayout;
        for (std::size_t i = 0; i < resident.mesh.surfaces.size(); ++i)
        {
            const auto& left = resident.mesh.surfaces[i];
            const auto& right = current.mesh.surfaces[i];
            if (left.topology != right.topology || left.firstIndex != right.firstIndex
                || left.indexCount != right.indexCount || left.materialSlot != right.materialSlot) return Reason::Surfaces;
        }
        for (std::size_t i = 0; i < resident.textures.size(); ++i)
        {
            if (resident.textures[i].texture != current.textures[i].texture) return Reason::TextureIdentity;
            if (resident.textures[i].binding != current.textures[i].binding) return Reason::TextureBinding;
        }
        return Reason::None;
    }
    [[nodiscard]] inline bool immediateEffectLayoutMatches(
        const RenderCore::ImmediateEffectDraw& resident, const RenderCore::ImmediateEffectDraw& current,
        bool immutableBoundsControl = false) noexcept
    {
        return immediateEffectLayoutMismatch(resident, current, immutableBoundsControl) == ImmediateEffectMismatch::None;
    }

}
#endif
