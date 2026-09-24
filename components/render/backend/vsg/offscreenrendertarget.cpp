#include "offscreenrendertarget.hpp"

#include <vsg/app/RenderGraph.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/PhysicalDevice.h>
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

        [[nodiscard]] VkFormat colorFormat(RenderCore::RenderTargetFormat format) noexcept
        {
            switch (format)
            {
                case RenderCore::RenderTargetFormat::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
                case RenderCore::RenderTargetFormat::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
                case RenderCore::RenderTargetFormat::SurfaceColor:
                case RenderCore::RenderTargetFormat::Depth32Float: return VK_FORMAT_UNDEFINED;
            }
            return VK_FORMAT_UNDEFINED;
        }
    }

    void OffscreenRenderTarget::setClearValues(VkClearColorValue clearColor, VkClearDepthStencilValue clearDepth)
    {
        if (!renderGraph)
            return;
        renderGraph->clearValues.resize(depthFormat ? 2u : 1u);
        renderGraph->clearValues[0].color = clearColor;
        if (depthFormat)
            renderGraph->clearValues[1].depthStencil = clearDepth;
    }

    OffscreenRenderTarget createOffscreenRenderTarget(vsg::Device* device, RenderCore::Extent2D extent,
        RenderCore::RenderTargetFormat requestedColorFormat,
        std::optional<RenderCore::RenderTargetFormat> requestedDepthFormat, bool sampleDepth)
    {
        OffscreenRenderTarget result;
        if (!device || !extent.valid() || (sampleDepth && !requestedDepthFormat))
            return result;

        const VkFormat colorVk = colorFormat(requestedColorFormat);
        if (colorVk == VK_FORMAT_UNDEFINED
            || (requestedDepthFormat && *requestedDepthFormat != RenderCore::RenderTargetFormat::Depth32Float))
            return result;

        if (sampleDepth)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device->getPhysicalDevice()->vk(), VK_FORMAT_D32_SFLOAT, &properties);
            constexpr VkFormatFeatureFlags required
                = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((properties.optimalTilingFeatures & required) != required)
                return {};
        }

        // Persistent auxiliary surfaces can be sampled by MyGUI and, for map
        // persistence, copied to a host-visible staging buffer after GPU
        // completion. TRANSFER_SRC does not change the normal render/sampled
        // lifetime; it only makes the explicit readback path legal.
        result.color = createAttachment(device, extent, colorVk,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        if (!result.color)
            return {};

        constexpr VkFormat depthVk = VK_FORMAT_D32_SFLOAT;
        if (requestedDepthFormat)
        {
            result.depth = createAttachment(
                device, extent, depthVk, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | (sampleDepth ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VkImageUsageFlagBits{}), VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!result.depth)
                return {};
        }

        vsg::AttachmentDescription color = vsg::defaultColorAttachment(colorVk);
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vsg::RenderPass::Attachments attachments{ color };
        vsg::ImageViews imageViews{ result.color };
        vsg::SubpassDescription subpass;
        subpass.colorAttachments = { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT } };
        if (requestedDepthFormat)
        {
            vsg::AttachmentDescription depth = vsg::defaultDepthAttachment(depthVk);
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = sampleDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth.finalLayout = sampleDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(depth);
            imageViews.push_back(result.depth);
            subpass.depthStencilAttachments
                = { { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT } };
        }

        // A sampled depth attachment has both early and late depth writes.
        // Cover them before the water pass samples, and cover next-frame reuse.
        const VkPipelineStageFlags writes = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | (sampleDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0u);
        const VkAccessFlags writeAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | (sampleDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VkAccessFlagBits{});
        vsg::RenderPass::Dependencies dependencies{
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                writes, VK_ACCESS_SHADER_READ_BIT,
                writeAccess, VK_DEPENDENCY_BY_REGION_BIT },
            { 0, VK_SUBPASS_EXTERNAL, writes,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, writeAccess,
                VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT },
        };
        auto renderPass = vsg::RenderPass::create(
            device, attachments, vsg::RenderPass::Subpasses{ subpass }, dependencies);
        auto framebuffer = vsg::Framebuffer::create(renderPass, imageViews, extent.width, extent.height, 1);
        if (!renderPass || !framebuffer)
            return {};

        result.renderGraph = vsg::RenderGraph::create();
        result.renderGraph->framebuffer = std::move(framebuffer);
        result.renderGraph->renderArea = { { 0, 0 }, { extent.width, extent.height } };
        result.extent = extent;
        result.colorFormat = requestedColorFormat;
        result.depthFormat = requestedDepthFormat;
        result.setClearValues({ { 0.0f, 0.0f, 0.0f, 1.0f } }, { 0.0f, 0 });
        return result;
    }
}
