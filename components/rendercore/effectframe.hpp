#ifndef OPENMW_COMPONENTS_RENDERCORE_EFFECTFRAME_H
#define OPENMW_COMPONENTS_RENDERCORE_EFFECTFRAME_H

#include "records.hpp"
#include "boundedparallelfor.hpp"

#include <cstdint>
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    // Own the copy privately: a shared_ptr<const MeshPayload> alone still
    // permits a producer retaining a mutable alias to change a published frame.
    class FrozenEffectMesh final
    {
    public:
        explicit FrozenEffectMesh(const MeshPayload& source) : mMesh(source)
        {
            mValid = validMeshPayload(mMesh) && !mMesh.positions.empty() && !mMesh.surfaces.empty()
                && std::all_of(mMesh.surfaces.begin(), mMesh.surfaces.end(),
                    [](const auto& surface) { return surface.materialSlot == 0; });
            if (mValid)
            {
                mBounds.minimum = mBounds.maximum = mMesh.positions.front();
                for (const auto& p : mMesh.positions)
                { mBounds.minimum = glm::min(mBounds.minimum, p); mBounds.maximum = glm::max(mBounds.maximum, p); }
            }
        }
        FrozenEffectMesh(const FrozenEffectMesh&) = delete;
        FrozenEffectMesh& operator=(const FrozenEffectMesh&) = delete;
        const MeshPayload& mesh() const noexcept { return mMesh; }
        bool valid() const noexcept { return mValid; }
        const AxisAlignedBounds& bounds() const noexcept { return mBounds; }
    private:
        const MeshPayload mMesh;
        bool mValid = false;
        AxisAlignedBounds mBounds;
    };

    struct ImmediateEffectDraw
    {
        std::string identity;
        glm::mat4 worldTransform{ 1.0f };
        MeshPayload mesh;
        std::shared_ptr<const FrozenEffectMesh> meshSnapshot;
        [[nodiscard]] const MeshPayload& meshData() const noexcept
        { return meshSnapshot ? meshSnapshot->mesh() : mesh; }
        AxisAlignedBounds bounds;
        // Particle quads need one billboard boundary per evaluated particle.
        // Keeping this semantic neutral lets the VSG compatibility realizer use
        // the same legacy billboard math in main/reflection/refraction views.
        std::optional<ModelBillboardMode> billboard;
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
        const auto& mesh = draw.meshData();
        const bool validMesh = draw.meshSnapshot ? draw.meshSnapshot->valid() : validMeshPayload(mesh);
        if (draw.identity.empty() || !semantic_detail::finite(draw.worldTransform) || !validMesh
            || mesh.positions.empty() || mesh.surfaces.empty() || !draw.material.textures.empty())
            return false;
        if (!semantic_detail::finite(draw.bounds.minimum) || !semantic_detail::finite(draw.bounds.maximum)
            || draw.bounds.minimum.x > draw.bounds.maximum.x || draw.bounds.minimum.y > draw.bounds.maximum.y
            || draw.bounds.minimum.z > draw.bounds.maximum.z)
            return false;
        for (const MeshSurface& surface : mesh.surfaces)
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

    // A real ownership boundary, not a const view of a mutable producer vector.
    // Deep-copy once (including procedural texture pixels) so retained producer
    // aliases cannot invalidate the cached validation. Session routing, prepare,
    // render and history commit share this object without recopying geometry.
    class OwnedImmediateEffects final
    {
    public:
        // A caller may retain a non-const shared_ptr before publishing it as
        // const. It must not be able to replace this snapshot through that alias.
        OwnedImmediateEffects(const OwnedImmediateEffects&) = delete;
        OwnedImmediateEffects& operator=(const OwnedImmediateEffects&) = delete;
        OwnedImmediateEffects(OwnedImmediateEffects&&) = delete;
        OwnedImmediateEffects& operator=(OwnedImmediateEffects&&) = delete;

        explicit OwnedImmediateEffects(
            std::span<const ImmediateEffectDraw> source, BoundedParallelFor* workers = nullptr)
            : mDraws(source.size())
            , mPublicationWorkers(workers ? workers->workersFor(source.size()) : 0)
        {
            // Only read already-evaluated neutral data. Each job exclusively
            // owns one output element; all jobs finish before publication.
            // Do not use vector<bool>: adjacent result bits would race.
            std::vector<unsigned char> valid(source.size());
            const auto copyAndValidate = [&](std::size_t i) {
                auto& draw = mDraws[i];
                draw = source[i];
                for (auto& texture : draw.textures)
                    if (texture.texture.pixels)
                        texture.texture.pixels = std::make_shared<const TexturePixels>(*texture.texture.pixels);
                valid[i] = validImmediateEffectDraw(draw);
            };
            if (workers)
                workers->forEach(source.size(), copyAndValidate);
            else
                for (std::size_t i = 0; i < source.size(); ++i)
                    copyAndValidate(i);
            std::vector<std::string_view> identities;
            identities.reserve(mDraws.size());
            for (std::size_t i = 0; i < mDraws.size(); ++i)
            {
                const auto& draw = mDraws[i];
                mValid = mValid && valid[i];
                identities.push_back(draw.identity);
                mPayloadBytes += draw.meshData().positions.size() * sizeof(glm::vec3)
                    + draw.meshData().normals.size() * sizeof(glm::vec3)
                    + draw.meshData().colors.size() * sizeof(glm::vec4)
                    + draw.meshData().tangents.size() * sizeof(glm::vec3)
                    + draw.meshData().bitangents.size() * sizeof(glm::vec3)
                    + draw.meshData().indices.size() * sizeof(std::uint32_t);
                for (const auto& uv : draw.meshData().texCoordSets)
                    mPayloadBytes += uv.size() * sizeof(glm::vec2);
            }
            std::sort(identities.begin(), identities.end());
            mValid = mValid && std::adjacent_find(identities.begin(), identities.end()) == identities.end();
        }

        [[nodiscard]] bool valid() const noexcept { return mValid; }
        [[nodiscard]] const std::vector<ImmediateEffectDraw>& draws() const noexcept { return mDraws; }
        // Logical geometry bytes referenced at publication (immutable cached
        // meshes are shared), not bytes copied or an RSS/unique-owner estimate.
        [[nodiscard]] std::uint64_t payloadBytes() const noexcept { return mPayloadBytes; }
        [[nodiscard]] std::size_t publicationWorkers() const noexcept { return mPublicationWorkers; }

    private:
        std::vector<ImmediateEffectDraw> mDraws;
        bool mValid = true;
        std::uint64_t mPayloadBytes = 0;
        std::size_t mPublicationWorkers = 0;
    };
}

#endif
