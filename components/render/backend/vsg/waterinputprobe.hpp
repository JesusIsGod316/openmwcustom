#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_WATERINPUTPROBE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_WATERINPUTPROBE_H

#include "offscreenrendertarget.hpp"
#include <vsg/all.h>
#include <glm/gtc/packing.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace RenderVsg
{
    // One-shot sparse copies appended AFTER the frame's render passes, on the
    // same queue. The caller retains this object and reads only after the
    // registered semantic frame completes. No independent submit or GPU wait.
    class WaterInputProbe
    {
    public:
        static constexpr unsigned Grid = 8, Samples = Grid * Grid;
        struct Summary
        {
            unsigned colorNonclear = 0, depthNonclear = 0, nonfinite = 0;
            glm::vec4 minimum{std::numeric_limits<float>::max()}, maximum{-std::numeric_limits<float>::max()};
            float depthMinimum = 1.f, depthMaximum = 0.f;
            std::uint64_t checksum = 14695981039346656037ull;
        };
        RenderCore::FrameId frame;
        std::string label;
        std::string context;
        RenderCore::Extent2D extent;
        glm::vec4 clear{};
        vsg::ref_ptr<vsg::Commands> commands = vsg::Commands::create();

        WaterInputProbe(vsg::ref_ptr<vsg::Device> device, const OffscreenRenderTarget& target,
            RenderCore::FrameId sourceFrame, std::string name, bool depth)
            : frame(sourceFrame), label(std::move(name)), extent(target.extent), mDevice(device), mDepth(depth)
        {
            if (!frame.valid() || !target || target.colorFormat != RenderCore::RenderTargetFormat::Rgba16Float
                || (depth && target.depthFormat != RenderCore::RenderTargetFormat::Depth32Float))
                throw std::runtime_error("water probe requires a valid RGBA16F target and frame");
            const auto& color = target.renderGraph->clearValues.at(0).color;
            clear = {color.float32[0], color.float32[1], color.float32[2], color.float32[3]};
            mColorBuffer = addCopy(target.color, false);
            if (depth) mDepthBuffer = addCopy(target.depth, true);
        }

        std::optional<Summary> read(RenderCore::FrameId completed) const
        {
            if (!completed.valid() || completed < frame) return std::nullopt;
            Summary result;
            const auto colors = map(mColorBuffer, Samples * 8);
            for (unsigned i = 0; i < Samples; ++i)
            {
                std::array<std::uint16_t,4> packed;
                std::memcpy(packed.data(), static_cast<const char*>(colors->dataPointer()) + i*8, 8);
                glm::vec4 pixel;
                bool differs = false;
                for (unsigned channel = 0; channel < 4; ++channel)
                {
                    pixel[channel] = glm::unpackHalf1x16(packed[channel]);
                    if (!std::isfinite(pixel[channel])) ++result.nonfinite;
                    else
                    {
                        result.minimum[channel] = std::min(result.minimum[channel], pixel[channel]);
                        result.maximum[channel] = std::max(result.maximum[channel], pixel[channel]);
                        differs |= std::abs(pixel[channel] - clear[channel]) > .002f;
                    }
                }
                result.colorNonclear += differs;
            }
            hash(result.checksum, *colors);
            if (mDepth)
            {
                const auto depths = map(mDepthBuffer, Samples * 4);
                for (unsigned i = 0; i < Samples; ++i)
                {
                    float value;
                    std::memcpy(&value, static_cast<const char*>(depths->dataPointer()) + i*4, 4);
                    if (!std::isfinite(value)) ++result.nonfinite;
                    else
                    {
                        result.depthMinimum = std::min(result.depthMinimum, value);
                        result.depthMaximum = std::max(result.depthMaximum, value);
                        result.depthNonclear += value != 0.f; // reversed-depth clear
                    }
                }
                hash(result.checksum, *depths);
            }
            return result;
        }
        bool hasDepth() const noexcept { return mDepth; }
    private:
        vsg::ref_ptr<vsg::Device> mDevice;
        bool mDepth;
        vsg::ref_ptr<vsg::Buffer> mColorBuffer, mDepthBuffer;

        vsg::ref_ptr<vsg::Buffer> addCopy(vsg::ref_ptr<vsg::ImageView> view, bool depth)
        {
            if (!view || !view->image || !(view->image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                || view->image->format != (depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT)
                || view->image->samples != VK_SAMPLE_COUNT_1_BIT)
                throw std::runtime_error("water probe target is not transfer-readable");
            const unsigned stride = depth ? 4u : 8u;
            auto buffer = vsg::createBufferAndMemory(mDevice, Samples * stride, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_SHARING_MODE_EXCLUSIVE, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!buffer) throw std::runtime_error("water probe staging allocation failed");
            const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            const VkImageSubresourceRange range{aspect,0,1,0,1};
            const auto layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const auto writes = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            const auto stages = depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            commands->addChild(vsg::PipelineBarrier::create(stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, vsg::ImageMemoryBarrier::create(writes | VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, view->image, range)));
            auto copy = vsg::CopyImageToBuffer::create();
            copy->srcImage = view->image; copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; copy->dstBuffer = buffer;
            for (unsigned y=0; y<Grid; ++y) for (unsigned x=0; x<Grid; ++x)
            {
                VkBufferImageCopy region{};
                region.bufferOffset = (y*Grid+x)*stride;
                region.imageSubresource = {aspect,0,0,1};
                region.imageOffset = {int((2*x+1)*extent.width/(2*Grid)),int((2*y+1)*extent.height/(2*Grid)),0};
                region.imageExtent = {1,1,1}; copy->regions.push_back(region);
            }
            commands->addChild(copy);
            commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, view->image, range)));
            commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                vsg::BufferMemoryBarrier::create(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buffer, 0, Samples*stride)));
            return buffer;
        }
        vsg::ref_ptr<vsg::MappedData<vsg::ubyteArray>> map(vsg::ref_ptr<vsg::Buffer> buffer, unsigned bytes) const
        {
            auto memory = buffer->getDeviceMemory(mDevice->deviceID);
            if (!memory) throw std::runtime_error("water probe staging memory missing");
            auto mapped = vsg::MappedData<vsg::ubyteArray>::create(memory, buffer->getMemoryOffset(mDevice->deviceID),
                0, vsg::Data::Properties{}, bytes);
            if (!mapped || !mapped->dataPointer()) throw std::runtime_error("water probe mapping failed");
            return mapped;
        }
        static void hash(std::uint64_t& value, const vsg::ubyteArray& bytes)
        {
            for (auto byte : bytes) value = (value ^ byte) * 1099511628211ull;
        }
    };
}
#endif
