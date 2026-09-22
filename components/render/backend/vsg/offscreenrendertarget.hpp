#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OFFSCREENRENDERTARGET_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OFFSCREENRENDERTARGET_H

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/vulkan.h>

#include <optional>

namespace vsg
{
    class Device;
    class ImageView;
    class RenderGraph;
}

namespace RenderVsg
{
    struct OffscreenRenderTarget
    {
        vsg::ref_ptr<vsg::RenderGraph> renderGraph;
        vsg::ref_ptr<vsg::ImageView> color;
        vsg::ref_ptr<vsg::ImageView> depth;
        RenderCore::Extent2D extent;
        RenderCore::RenderTargetFormat colorFormat = RenderCore::RenderTargetFormat::Rgba16Float;
        std::optional<RenderCore::RenderTargetFormat> depthFormat = RenderCore::RenderTargetFormat::Depth32Float;

        // VSG 1.1.15 infers clear-value type from the final image layout, which
        // does not recognize sampled depth. Use the attachment contract instead.
        void setClearValues(VkClearColorValue clearColor, VkClearDepthStencilValue clearDepth);

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return renderGraph && color && extent.valid() && (!depthFormat || depth);
        }
    };

    [[nodiscard]] OffscreenRenderTarget createOffscreenRenderTarget(vsg::Device* device,
        RenderCore::Extent2D extent, RenderCore::RenderTargetFormat colorFormat,
        std::optional<RenderCore::RenderTargetFormat> depthFormat, bool sampleDepth = false);

    [[nodiscard]] inline OffscreenRenderTarget createOffscreenRenderTarget(
        vsg::Device* device, RenderCore::Extent2D extent)
    {
        return createOffscreenRenderTarget(device, extent, RenderCore::RenderTargetFormat::Rgba16Float,
            RenderCore::RenderTargetFormat::Depth32Float);
    }
}

#endif