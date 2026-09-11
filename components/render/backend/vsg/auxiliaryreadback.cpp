#include "vsgruntimehost.hpp"

#include <vsg/all.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace RenderVsg
{
    std::optional<std::vector<VsgRuntimeHost::AuxiliaryRgba8Readback>> VsgRuntimeHost::readbackAuxiliaryRgba8(
        std::span<const RenderCore::RenderTargetHandle> targets)
    {
        if (targets.empty())
            return std::vector<AuxiliaryRgba8Readback>{};

        struct PendingReadback
        {
            RenderCore::RenderTargetHandle identity;
            RenderCore::Extent2D extent;
            vsg::ref_ptr<vsg::Buffer> buffer;
            std::size_t byteCount = 0;
        };

        std::vector<PendingReadback> pending;
        pending.reserve(targets.size());

        vsg::ref_ptr<vsg::Device> device = mWindow ? mWindow->getOrCreateDevice() : vsg::ref_ptr<vsg::Device>{};
        if (!device)
        {
            mLastDiagnostic = "auxiliary RGBA readback has no Vulkan device";
            return std::nullopt;
        }

        auto commands = vsg::Commands::create();
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            const RenderCore::RenderTargetHandle identity = targets[index];
            if (std::find(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(index), identity)
                != targets.begin() + static_cast<std::ptrdiff_t>(index))
            {
                mLastDiagnostic = "auxiliary RGBA readback contains a duplicate target";
                return std::nullopt;
            }

            const auto runtime = std::find_if(mAuxiliaryViews.begin(), mAuxiliaryViews.end(),
                [&](const AuxiliaryViewRuntime& value) { return value.targetIdentity == identity; });
            if (runtime == mAuxiliaryViews.end() || !runtime->target.color || !runtime->target.color->image
                || runtime->colorFormat != RenderCore::RenderTargetFormat::Rgba8Srgb
                || !runtime->target.extent.valid())
            {
                mLastDiagnostic = "auxiliary RGBA readback target is absent or not RGBA8 sRGB";
                return std::nullopt;
            }

            const std::uint64_t pixels
                = static_cast<std::uint64_t>(runtime->target.extent.width) * runtime->target.extent.height;
            if (pixels > std::numeric_limits<std::size_t>::max() / 4u)
            {
                mLastDiagnostic = "auxiliary RGBA readback byte count overflowed host size_t";
                return std::nullopt;
            }
            const std::size_t byteCount = static_cast<std::size_t>(pixels) * 4u;
            auto buffer = vsg::createBufferAndMemory(device, static_cast<VkDeviceSize>(byteCount),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!buffer)
            {
                mLastDiagnostic = "auxiliary RGBA readback could not allocate a host-visible staging buffer";
                return std::nullopt;
            }

            vsg::ref_ptr<vsg::Image> source = runtime->target.color->image;
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            auto toTransfer = vsg::ImageMemoryBarrier::create(VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                source, range);
            commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toTransfer));

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { runtime->target.extent.width, runtime->target.extent.height, 1 };

            auto copy = vsg::CopyImageToBuffer::create();
            copy->srcImage = source;
            copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy->dstBuffer = buffer;
            copy->regions.push_back(region);
            commands->addChild(copy);

            auto toSampled = vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                source, range);
            commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, toSampled));

            pending.push_back(PendingReadback{ identity, runtime->target.extent, std::move(buffer), byteCount });
        }

        // The map/save readback is deliberately outside the frame submission
        // path. Synchronize once, then copy every requested surface in a single
        // short queue submission so N visible map cells do not cause N device
        // idle waits or N fence round trips.
        waitIdle();
        vsg::ref_ptr<vsg::PhysicalDevice> physicalDevice = device->getPhysicalDevice();
        if (!physicalDevice)
        {
            mLastDiagnostic = "auxiliary RGBA readback has no Vulkan physical device";
            return std::nullopt;
        }
        const std::uint32_t queueFamilyIndex = physicalDevice->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
        vsg::ref_ptr<vsg::Queue> queue = device->getQueue(queueFamilyIndex);
        if (!queue)
        {
            mLastDiagnostic = "auxiliary RGBA readback could not resolve the graphics queue";
            return std::nullopt;
        }
        auto commandPool = vsg::CommandPool::create(device, queueFamilyIndex);
        auto fence = vsg::Fence::create(device);
        if (!commandPool || !fence)
        {
            mLastDiagnostic = "auxiliary RGBA readback could not allocate submission synchronization";
            return std::nullopt;
        }

        constexpr std::uint64_t ReadbackTimeoutNs = 100000000000ull;
        const VkResult submit = vsg::submitCommandsToQueue(
            commandPool, fence, ReadbackTimeoutNs, queue, [&](vsg::CommandBuffer& commandBuffer) {
                commands->record(commandBuffer);
            });
        if (submit != VK_SUCCESS)
        {
            mLastDiagnostic = "auxiliary RGBA readback queue submission failed with VkResult "
                + std::to_string(submit);
            return std::nullopt;
        }

        std::vector<AuxiliaryRgba8Readback> result;
        result.reserve(pending.size());
        for (PendingReadback& value : pending)
        {
            vsg::DeviceMemory* const memory = value.buffer->getDeviceMemory(device->deviceID);
            if (!memory)
            {
                mLastDiagnostic = "auxiliary RGBA readback staging buffer lost its device memory";
                return std::nullopt;
            }
            const VkDeviceSize offset = value.buffer->getMemoryOffset(device->deviceID);
            auto mapped = vsg::MappedData<vsg::ubyteArray>::create(memory, offset, 0,
                vsg::Data::Properties{ VK_FORMAT_R8G8B8A8_SRGB }, value.byteCount);
            if (!mapped || mapped->size() < value.byteCount)
            {
                mLastDiagnostic = "auxiliary RGBA readback could not map the complete staging buffer";
                return std::nullopt;
            }

            AuxiliaryRgba8Readback image;
            image.target = value.identity;
            image.extent = value.extent;
            image.rgba.resize(value.byteCount);
            std::memcpy(image.rgba.data(), mapped->dataPointer(), value.byteCount);
            result.push_back(std::move(image));
        }
        return result;
    }
}
