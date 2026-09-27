#ifndef OPENMW_RENDER_VSG_FXIMAGECAPTURE_HPP
#define OPENMW_RENDER_VSG_FXIMAGECAPTURE_HPP
#include "fximagestate.hpp"
#include <osg/Image>
#include <osgDB/WriteFile>
#include <glm/gtc/packing.hpp>
#include <filesystem>
#include <cstring>
#include <cmath>

namespace RenderVsg
{
    // Explicit visual-test-only readback. Caller must wait for the submitted
    // frame first. Never enable during a performance comparison: this stalls.
    inline void captureFxImage(vsg::Device* device, vsg::ImageView* view, const std::filesystem::path& path)
    {
        auto source = view->image;
        if (source->format != VK_FORMAT_R16G16B16A16_SFLOAT
            || !(source->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            throw std::runtime_error("FX capture requires transfer-readable RGBA16F");
        const auto width = source->extent.width, height = source->extent.height;
        const auto size = VkDeviceSize(width)*height*8;
        auto buffer = vsg::createBufferAndMemory(device,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_SHARING_MODE_EXCLUSIVE,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const auto family = device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
        auto pool = vsg::CommandPool::create(device,family);
        auto fence = vsg::Fence::create(device);
        auto copy = vsg::CopyImageToBuffer::create();
        copy->srcImage=source;copy->srcImageLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;copy->dstBuffer=buffer;
        VkBufferImageCopy region{};
        region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={width,height,1};
        copy->regions.push_back(region);
        const auto result = vsg::submitCommandsToQueue(pool,fence,10000000000ull,device->getQueue(family),
            [&](vsg::CommandBuffer& command)
            {
                fxImageBarrier(command,*source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                copy->record(command);
                fxImageBarrier(command,*source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,FxSampleStages);
                auto barrier=vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,
                    vsg::BufferMemoryBarrier::create(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,
                        VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,buffer,0,size));
                barrier->record(command);
            });
        if (result != VK_SUCCESS) throw std::runtime_error("FX image readback failed");
        auto mapped=vsg::MappedData<vsg::ubyteArray>::create(buffer->getDeviceMemory(device->deviceID),
            buffer->getMemoryOffset(device->deviceID),0,vsg::Data::Properties{},static_cast<unsigned>(size));
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width,height,1,GL_RGBA,GL_UNSIGNED_BYTE);
        // osgDB's PNG writer expects bottom-up rows. Vulkan readback is top-down.
        for (std::size_t i=0; i<std::size_t(width)*height; ++i)
        {
            std::uint16_t pixel[4];std::memcpy(pixel,static_cast<const char*>(mapped->dataPointer())+i*8,8);
            for (unsigned c=0;c<4;++c)
            {
                float value=glm::unpackHalf1x16(pixel[c]);
                value=std::isfinite(value)?std::clamp(value,0.f,1.f):0.f;
                if(c<3)value=value<=.0031308f?12.92f*value:1.055f*std::pow(value,1.f/2.4f)-.055f;
                const auto flipped=(std::size_t(height)-1-i/width)*width+i%width;
                image->data()[flipped*4+c]=static_cast<unsigned char>(std::round(value*255));
            }
        }
        if(!osgDB::writeImageFile(*image,path.string()))throw std::runtime_error("FX image output failed");
    }
}
#endif
