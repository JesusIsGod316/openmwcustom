#include "offscreenrendertarget.hpp"

#include <vsg/app/RenderGraph.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] vsg::ref_ptr<vsg::ImageView> createAttachment(vsg::Device* device,
            RenderCore::Extent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
        {
            auto image = vsg::Image::create();
            image->imageType = VK_IMAGE_TYPE_2D;
            image->format = format;
            image->extent = { extent.width, extent.height, 1 };
            image->mipLevels = 1;
            image->arrayLayers = 1;
            image->samples = VK_SAMPLE_COUNT_1_BIT;
            image->tiling = VK_IMAGE_TILING_OPTIMAL;
            image->usage = usage;
            image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return vsg::createImageView(device, image, aspect);
        }
    }

    OffscreenRenderTarget createOffscreenRenderTarget(vsg::Device* device, RenderCore::Extent2D extent)
    {
        OffscreenRenderTarget result;
        if (!device || !extent.valid())
            return result;

        constexpr VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        result.color = createAttachment(device, extent, colorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        result.depth = createAttachment(
            device, extent, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!result.color || !result.depth)
            return {};

        vsg::AttachmentDescription color = vsg::defaultColorAttachment(colorFormat);
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vsg::AttachmentDescription depth = vsg::defaultDepthAttachment(depthFormat);
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        vsg::SubpassDescription subpass;
        subpass.colorAttachments = { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT } };
        subpass.depthStencilAttachments
            = { { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT } };
        vsg::RenderPass::Dependencies dependencies{
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_DEPENDENCY_BY_REGION_BIT },
            { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT },
        };
        auto renderPass = vsg::RenderPass::create(device, vsg::RenderPass::Attachments{ color, depth },
            vsg::RenderPass::Subpasses{ subpass }, dependencies);
        auto framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{ result.color, result.depth },
            extent.width, extent.height, 1);
        if (!renderPass || !framebuffer)
            return {};

        result.renderGraph = vsg::RenderGraph::create();
        result.renderGraph->framebuffer = std::move(framebuffer);
        result.renderGraph->renderArea = { { 0, 0 }, { extent.width, extent.height } };
        result.renderGraph->setClearValues({ { 0.0f, 0.0f, 0.0f, 1.0f } }, { 0.0f, 0 });
        result.extent = extent;
        return result;
    }
}
