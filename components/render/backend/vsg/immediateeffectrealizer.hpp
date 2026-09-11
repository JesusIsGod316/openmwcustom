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

        for (std::size_t i = 0; i < draw.textures.size(); ++i)
        {
            const EffectTextureSnapshot& snapshot = draw.textures[i];
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
        ModelNodeRecord node;
        node.name = draw.identity;
        node.kind = ModelNodeKind::Geometry;
        node.mesh = *meshHandle;
        node.materials = { *materialHandle };
        payload->nodes.push_back(std::move(node));
        payload->roots.emplace_back(0u);

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

        StaticRealizationResult realized
            = realizeStaticAssetConformant(world, *modelHandle, *plan, textureResolver, std::move(sharedObjects));
        if (!realized.valid() || realized.stats.runtimeContextEffects != 0
            || realized.stats.unsupportedTextureBindings != 0)
        {
            result.diagnostic = realized.diagnostics.empty()
                ? "evaluated effect requires an unsupported legacy realization semantic"
                : realized.diagnostics.front();
            return result;
        }
        result.root = std::move(realized.root);
        return result;
    }
}

#endif
