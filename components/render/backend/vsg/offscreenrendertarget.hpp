#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OFFSCREENRENDERTARGET_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OFFSCREENRENDERTARGET_H

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/ref_ptr.h>

namespace vsg
{
    class Device;
    class ImageView;
    class RenderGraph;
}

namespace RenderVsg
{
    // Persistent color/depth target used by CP4E reflection, refraction, map,
    // and preview views. Construction allocates once; frames only update the
    // attached camera and clear state.
    struct OffscreenRenderTarget
    {
        vsg::ref_ptr<vsg::RenderGraph> renderGraph;
        vsg::ref_ptr<vsg::ImageView> color;
        vsg::ref_ptr<vsg::ImageView> depth;
        RenderCore::Extent2D extent;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return renderGraph && color && depth && extent.valid();
        }
    };

    [[nodiscard]] OffscreenRenderTarget createOffscreenRenderTarget(
        vsg::Device* device, RenderCore::Extent2D extent);
}

#endif
