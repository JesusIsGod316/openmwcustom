#ifndef OPENMW_RENDER_VSG_FXIMAGESTATE_HPP
#define OPENMW_RENDER_VSG_FXIMAGESTATE_HPP

#include <vsg/all.h>

namespace RenderVsg
{
    inline void fxImageBarrier(vsg::CommandBuffer& command, vsg::Image& image,
        VkImageLayout before, VkImageLayout after, VkAccessFlags source, VkAccessFlags destination,
        VkPipelineStageFlags from, VkPipelineStageFlags to, unsigned base = 0, unsigned levels = 1)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.vk(command.deviceID);
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
        vkCmdPipelineBarrier(command, from, to, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    constexpr VkPipelineStageFlags FxSampleStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    class FxInitializeImage final : public vsg::Inherit<vsg::Command, FxInitializeImage>
    {
    public:
        FxInitializeImage(vsg::ref_ptr<vsg::Image> value, VkClearColorValue color) : image(value), clear(color) {}
        void record(vsg::CommandBuffer& command) const override
        {
            if (initialized) return;
            fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, image->mipLevels);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, image->mipLevels, 0, 1};
            vkCmdClearColorImage(command, image->vk(command.deviceID), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FxSampleStages, 0, image->mipLevels);
            initialized = true;
        }
        vsg::ref_ptr<vsg::Image> image;
        VkClearColorValue clear;
        mutable bool initialized = false;
    };

    class FxCopyImage final : public vsg::Inherit<vsg::Command, FxCopyImage>
    {
    public:
        FxCopyImage(vsg::ref_ptr<vsg::Image> src, vsg::ref_ptr<vsg::Image> dst) : source(src), destination(dst) {}
        void record(vsg::CommandBuffer& command) const override
        {
            fxImageBarrier(command, *source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, FxSampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, source->mipLevels);
            fxImageBarrier(command, *destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, FxSampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, destination->mipLevels);
            for (unsigned mip = 0; mip < source->mipLevels; ++mip)
            {
                VkImageCopy region{};
                region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
                region.extent = {std::max(1u, source->extent.width >> mip), std::max(1u, source->extent.height >> mip), 1};
                vkCmdCopyImage(command, source->vk(command.deviceID), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destination->vk(command.deviceID), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }
            fxImageBarrier(command, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FxSampleStages, 0, source->mipLevels);
            fxImageBarrier(command, *destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FxSampleStages, 0, destination->mipLevels);
        }
        vsg::ref_ptr<vsg::Image> source, destination;
    };

    class FxGenerateMipmaps final : public vsg::Inherit<vsg::Command, FxGenerateMipmaps>
    {
    public:
        explicit FxGenerateMipmaps(vsg::ref_ptr<vsg::Image> value) : image(value) {}
        void record(vsg::CommandBuffer& command) const override
        {
            for (unsigned mip = 1; mip < image->mipLevels; ++mip)
            {
                fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, FxSampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT, mip - 1);
                fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, FxSampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT, mip);
                VkImageBlit region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
                region.srcOffsets[1] = {static_cast<int>(std::max(1u, image->extent.width >> (mip - 1))),
                    static_cast<int>(std::max(1u, image->extent.height >> (mip - 1))), 1};
                region.dstOffsets[1] = {static_cast<int>(std::max(1u, image->extent.width >> mip)),
                    static_cast<int>(std::max(1u, image->extent.height >> mip)), 1};
                vkCmdBlitImage(command, image->vk(command.deviceID), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    image->vk(command.deviceID), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
                fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FxSampleStages, mip - 1);
                fxImageBarrier(command, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FxSampleStages, mip);
            }
        }
        vsg::ref_ptr<vsg::Image> image;
    };
}
#endif
