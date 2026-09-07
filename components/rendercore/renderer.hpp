#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERER_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERER_H

#include "backendselection.hpp"
#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <cstdint>

namespace RenderCore
{
    // Renderer entry coherence gate. Backends must not consume frame-local state
    // against a different logical world revision, or dereference dynamic handles
    // that were retired/replaced after the frame snapshot was assembled.
    [[nodiscard]] inline bool frameCompatibleWithWorld(
        const RenderWorld& world, const FrameRenderState& frame) noexcept
    {
        if (!frame.valid() || frame.worldEpoch() != world.epoch()
            || frame.renderWorldRevision() != world.revision())
            return false;

        for (const DynamicTransformState& transform : frame.dynamicTransforms())
        {
            if (!world.get(transform.instance))
                return false;
        }

        for (const DynamicMaterialState& state : frame.dynamicMaterials())
        {
            const MaterialRecord* material = world.get(state.material);
            if (!material)
                return false;
            for (const DynamicTextureTransformState& transform : state.textureTransforms)
            {
                if (transform.bindingIndex >= material->textures.size())
                    return false;
            }
        }
        return true;
    }

    enum class RenderFrameResult : std::uint8_t
    {
        Presented,
        Skipped,
        Failed,
    };

    // Deliberately narrow semantic backend seam. The game publishes logical world
    // and immutable frame state above this boundary; backend objects never cross it.
    class SemanticRenderer
    {
    public:
        virtual ~SemanticRenderer() = default;
        [[nodiscard]] virtual RenderBackendKind backendKind() const noexcept = 0;
        virtual RenderFrameResult renderFrame(const RenderWorld& world, const FrameRenderState& frame) = 0;
        virtual void waitIdle() = 0;
    };
}

#endif
