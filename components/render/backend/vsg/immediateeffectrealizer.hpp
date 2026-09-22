#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_IMMEDIATEEFFECTREALIZER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_IMMEDIATEEFFECTREALIZER_H

#include "staticassetconformance.hpp"

#include <components/rendercore/effectframe.hpp>
#include <components/rendercore/renderworld.hpp>

#include <memory>
#include <string>

namespace RenderVsg
{
    struct ImmediateEffectRealization
    {
        vsg::ref_ptr<vsg::Group> root;
        std::vector<StaticRealizationResult::MutableDrawStreams> mutableDraws;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(root); }
    };

    // Realize one low-volume, already-evaluated gameplay effect using the same
    // legacy material/texture path as persistent NIF resources. The temporary
    // RenderWorld exists only to provide typed handles while descriptors and VSG
    // objects are built; no transient gameplay state is published globally.
    [[nodiscard]] inline ImmediateEffectRealization realizeImmediateEffectDraw(
        const RenderCore::ImmediateEffectDraw& draw, const StaticTextureResolver& textureResolver,
        vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
    {
        using namespace RenderCore;

        ImmediateEffectRealization result;
        if (!validImmediateEffectDraw(draw) || !textureResolver)
        {
            result.diagnostic = "evaluated effect draw failed neutral validation";
            return result;
        }

        RenderWorld world;
        MaterialRecord material = draw.material;
        material.sourceIdentity = draw.identity + ":material";
        material.textures.clear();
        material.textures.reserve(draw.textures.size());

        for (const EffectTextureSnapshot& snapshot : draw.textures)
        {
            const std::optional<TextureHandle> handle = world.reserveTexture();
            if (!handle)
            {
                result.diagnostic = "evaluated effect texture handle reservation failed";
                return result;
            }
            TextureRecord texture = snapshot.texture;
            texture.revision = InitialResourceRevision;
            if (!world.commit(*handle, std::move(texture)))
            {
                result.diagnostic = "evaluated effect texture publication failed";
                return result;
            }
            TextureBinding binding = snapshot.binding;
            binding.texture = *handle;
            material.textures.push_back(std::move(binding));
        }

        const std::optional<MaterialHandle> materialHandle = world.reserveMaterial();
        if (!materialHandle)
        {
            result.diagnostic = "evaluated effect material handle reservation failed";
            return result;
        }
        material.revision = InitialResourceRevision;
        if (!world.commit(*materialHandle, std::move(material)))
        {
            result.diagnostic = "evaluated effect material publication failed";
            return result;
        }

        const std::optional<MeshHandle> meshHandle = world.reserveMesh();
        if (!meshHandle)
        {
            result.diagnostic = "evaluated effect mesh handle reservation failed";
            return result;
        }
        MeshRecord mesh;
        mesh.revision = InitialResourceRevision;
        mesh.sourceIdentity = draw.identity + ":mesh";
        mesh.bounds = draw.bounds;
        mesh.surfaceCount = static_cast<std::uint32_t>(draw.mesh.surfaces.size());
        mesh.payload = std::make_shared<const MeshPayload>(draw.mesh);
        if (!world.commit(*meshHandle, std::move(mesh)))
        {
            result.diagnostic = "evaluated effect mesh publication failed";
            return result;
        }

        auto payload = std::make_shared<ModelPayload>();
        ModelNodeRecord geometry;
        geometry.name = draw.identity + ":geometry";
        geometry.kind = ModelNodeKind::Geometry;
        geometry.mesh = *meshHandle;
        geometry.materials = { *materialHandle };
        if (draw.billboard)
        {
            ModelNodeRecord billboard;
            billboard.name = draw.identity + ":billboard";
            billboard.kind = ModelNodeKind::Billboard;
            billboard.billboard = draw.billboard;
            payload->nodes.push_back(std::move(billboard));
            geometry.parent = ModelNodeIndex{ 0u };
            payload->nodes.push_back(std::move(geometry));
            payload->roots.emplace_back(0u);
        }
        else
        {
            payload->nodes.push_back(std::move(geometry));
            payload->roots.emplace_back(0u);
        }

        const std::optional<ModelHandle> modelHandle = world.reserveModel();
        if (!modelHandle)
        {
            result.diagnostic = "evaluated effect model handle reservation failed";
            return result;
        }
        ModelRecord model;
        model.revision = InitialResourceRevision;
        model.sourceIdentity = draw.identity + ":model";
        model.contentIdentity = draw.identity + ":frame";
        model.bounds = draw.bounds;
        model.dynamicRequirements = 0;
        model.payload = std::move(payload);
        if (!world.commit(*modelHandle, std::move(model)))
        {
            result.diagnostic = "evaluated effect model publication failed";
            return result;
        }

        const std::optional<StaticAssetPlan> plan = buildStaticAssetPlan(world, *modelHandle);
        if (!plan)
        {
            result.diagnostic = "evaluated effect could not produce a legacy draw plan";
            return result;
        }

        StaticRealizationResult realized = realizeStaticAssetConformant(
            world, *modelHandle, *plan, textureResolver, std::move(sharedObjects), {}, {}, {}, 1.0f, true);
        if (!realized.valid() || realized.stats.runtimeContextEffects != 0
            || realized.stats.unsupportedTextureBindings != 0)
        {
            result.diagnostic = realized.diagnostics.empty()
                ? "evaluated effect requires an unsupported legacy realization semantic"
                : realized.diagnostics.front();
            return result;
        }
        result.root = std::move(realized.root);
        result.mutableDraws = std::move(realized.mutableDraws);
        return result;
    }

    [[nodiscard]] inline bool immediateEffectLayoutMatches(
        const RenderCore::ImmediateEffectDraw& resident, const RenderCore::ImmediateEffectDraw& current,
        bool immutableBoundsControl = false) noexcept
    {
        // Only streams updated below may differ. Matching sizes alone does not
        // make a resident index buffer, descriptor, uniform or bound current.
        // Defaulted semantic equality also includes future material fields.
        if (resident.identity != current.identity || resident.billboard != current.billboard
            || (immutableBoundsControl && (resident.bounds.minimum != current.bounds.minimum
                || resident.bounds.maximum != current.bounds.maximum))
            || resident.material != current.material
            || resident.semanticFlags != current.semanticFlags
            || resident.mesh.indices != current.mesh.indices
            || resident.mesh.tangents != current.mesh.tangents || resident.mesh.bitangents != current.mesh.bitangents
            || resident.mesh.positions.size() != current.mesh.positions.size()
            || resident.mesh.normals.size() != current.mesh.normals.size()
            || resident.mesh.colors.size() != current.mesh.colors.size()
            || resident.mesh.texCoordSets.size() != current.mesh.texCoordSets.size()
            || resident.mesh.surfaces.size() != current.mesh.surfaces.size()
            || resident.textures.size() != current.textures.size())
            return false;
        for (std::size_t i = 0; i < resident.mesh.texCoordSets.size(); ++i)
        {
            if (resident.mesh.texCoordSets[i].size() != current.mesh.texCoordSets[i].size())
                return false;
        }
        for (std::size_t i = 0; i < resident.mesh.surfaces.size(); ++i)
        {
            const auto& left = resident.mesh.surfaces[i];
            const auto& right = current.mesh.surfaces[i];
            if (left.topology != right.topology || left.firstIndex != right.firstIndex
                || left.indexCount != right.indexCount || left.materialSlot != right.materialSlot)
                return false;
        }
        for (std::size_t i = 0; i < resident.textures.size(); ++i)
        {
            if (resident.textures[i].texture != current.textures[i].texture
                || resident.textures[i].binding != current.textures[i].binding)
                return false;
        }
        return true;
    }

    [[nodiscard]] inline bool updateImmediateEffectRealization(const RenderCore::ImmediateEffectDraw& draw,
        std::vector<StaticRealizationResult::MutableDrawStreams>& mutableDraws) noexcept
    {
        if (!RenderCore::validImmediateEffectDraw(draw) || mutableDraws.size() != draw.mesh.surfaces.size())
            return false;
        // Validate the entire destination before changing any resident array.
        // A malformed later surface/UV stream must not leave earlier surfaces
        // half updated when the caller rejects this reuse attempt.
        for (const auto& streams : mutableDraws)
        {
            if (!streams.positions || !streams.normals || !streams.colors
                || streams.positions->size() != draw.mesh.positions.size()
                || streams.normals->size() != draw.mesh.positions.size()
                || streams.colors->size() != draw.mesh.positions.size()
                || streams.texCoords.size() != draw.mesh.texCoordSets.size())
                return false;
            for (std::size_t set = 0; set < streams.texCoords.size(); ++set)
            {
                if (!streams.texCoords[set]
                    || streams.texCoords[set]->size() != draw.mesh.texCoordSets[set].size())
                    return false;
            }
        }
        // Immediate effects have identity local model transforms; the caller
        // updates their outer world placement. Billboards rotate about their
        // origin, so use an origin-centred sphere as in billboardDrawBound.
        glm::dvec3 minimum(draw.mesh.positions.front()), maximum(minimum);
        double billboardRadius = 0.0;
        for (const auto& position : draw.mesh.positions)
        {
            const glm::dvec3 p(position);
            minimum = glm::min(minimum, p);
            maximum = glm::max(maximum, p);
            billboardRadius = std::max(billboardRadius, glm::length(p));
        }
        const glm::dvec3 center = draw.billboard ? glm::dvec3(0.0) : (minimum + maximum) * 0.5;
        const double radius = draw.billboard ? billboardRadius : glm::length(maximum - minimum) * 0.5;
        for (auto& streams : mutableDraws)
        {
            if (streams.sorted)
                streams.sorted->bound = vsg::dsphere(center.x, center.y, center.z, radius);
            bool positionsChanged = false;
            bool normalsChanged = false;
            bool colorsChanged = false;
            for (std::size_t i = 0; i < draw.mesh.positions.size(); ++i)
            {
                const glm::vec3& position = draw.mesh.positions[i];
                const vsg::vec3 nextPosition(position.x, position.y, position.z);
                if ((*streams.positions)[i] != nextPosition)
                {
                    streams.positions->set(i, nextPosition);
                    positionsChanged = true;
                }
                const glm::vec3 normal = draw.mesh.normals.empty()
                    ? glm::vec3(0.0f, 0.0f, 1.0f) : draw.mesh.normals[i];
                const vsg::vec3 nextNormal(normal.x, normal.y, normal.z);
                if ((*streams.normals)[i] != nextNormal)
                {
                    streams.normals->set(i, nextNormal);
                    normalsChanged = true;
                }
                const glm::vec4 color = draw.mesh.colors.empty() ? glm::vec4(1.0f) : draw.mesh.colors[i];
                const vsg::vec4 nextColor(color.x, color.y, color.z, color.w);
                if ((*streams.colors)[i] != nextColor)
                {
                    streams.colors->set(i, nextColor);
                    colorsChanged = true;
                }
            }
            // VSG uploads dirty DYNAMIC_DATA streams. Placement-only changes
            // must not trigger uploads of identical mesh, color and UV data.
            if (positionsChanged)
                streams.positions->dirty();
            if (normalsChanged)
                streams.normals->dirty();
            if (colorsChanged)
                streams.colors->dirty();
            for (std::size_t set = 0; set < streams.texCoords.size(); ++set)
            {
                bool changed = false;
                for (std::size_t i = 0; i < draw.mesh.texCoordSets[set].size(); ++i)
                {
                    const glm::vec2& uv = draw.mesh.texCoordSets[set][i];
                    const vsg::vec2 nextUv(uv.x, uv.y);
                    if ((*streams.texCoords[set])[i] != nextUv)
                    {
                        streams.texCoords[set]->set(i, nextUv);
                        changed = true;
                    }
                }
                if (changed)
                    streams.texCoords[set]->dirty();
            }
        }
        return true;
    }
}

#endif
