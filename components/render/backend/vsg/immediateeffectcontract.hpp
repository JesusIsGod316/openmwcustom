#ifndef OPENMW_RENDER_VSG_IMMEDIATEEFFECTCONTRACT_H
#define OPENMW_RENDER_VSG_IMMEDIATEEFFECTCONTRACT_H
#include <components/rendercore/effectframe.hpp>
#include <components/misc/environmentflag.hpp>
#include <cstddef>
#include <cstdlib>
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
        // FrozenEffectMesh owns a private immutable copy, so owner identity
        // proves all topology/layout/tangent predicates. Material and texture
        // values remain independently checked on every reuse.
        const bool sameMesh = Misc::environmentFlag<"OPENMW_V4_INCREMENTAL_CAPTURE">() && resident.meshSnapshot
            && resident.meshSnapshot == current.meshSnapshot;
        if (!sameMesh)
        {
            if (resident.meshData().indices != current.meshData().indices) return Reason::Indices;
            if (resident.meshData().tangents != current.meshData().tangents
                || resident.meshData().bitangents != current.meshData().bitangents) return Reason::TangentBasis;
            if (resident.meshData().positions.size() != current.meshData().positions.size()
                || resident.meshData().normals.size() != current.meshData().normals.size()
                || resident.meshData().colors.size() != current.meshData().colors.size()) return Reason::VertexLayout;
            if (resident.meshData().texCoordSets.size() != current.meshData().texCoordSets.size()) return Reason::UvLayout;
            if (resident.meshData().surfaces.size() != current.meshData().surfaces.size()) return Reason::Surfaces;
            for (std::size_t i = 0; i < resident.meshData().texCoordSets.size(); ++i)
                if (resident.meshData().texCoordSets[i].size() != current.meshData().texCoordSets[i].size())
                    return Reason::UvLayout;
            for (std::size_t i = 0; i < resident.meshData().surfaces.size(); ++i)
            {
                const auto& left = resident.meshData().surfaces[i];
                const auto& right = current.meshData().surfaces[i];
                if (left.topology != right.topology || left.firstIndex != right.firstIndex
                    || left.indexCount != right.indexCount || left.materialSlot != right.materialSlot)
                    return Reason::Surfaces;
            }
        }
        if (resident.textures.size() != current.textures.size()) return Reason::TextureCount;
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
