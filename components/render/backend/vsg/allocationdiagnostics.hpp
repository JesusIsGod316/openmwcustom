#ifndef OPENMW_RENDER_VSG_ALLOCATIONDIAGNOSTICS_H
#define OPENMW_RENDER_VSG_ALLOCATIONDIAGNOSTICS_H

#include <vsg/vk/MemoryBufferPools.h>
#include <vsg/vk/ResourceRequirements.h>
#include <sstream>
#include <unordered_set>

namespace RenderVsg
{
    // On-demand census, never a per-object log or a memory-budget claim. Pool
    // totals exclude driver/pipeline allocations; payload bytes exclude image
    // tiling/alignment/generated mipmaps. Report those distinctions explicitly.
    inline std::string allocationSummary(const vsg::Object& root, vsg::Device& device)
    {
        vsg::CollectResourceRequirements collect;
        root.accept(collect);
        std::unordered_set<const vsg::Image*> images;
        std::unordered_set<const vsg::Data*> imageData;
        std::unordered_set<const vsg::BufferInfo*> buffers;
        std::unordered_set<const vsg::Data*> bufferData, pendingBufferData, pendingImageData;
        std::uint64_t imageBytes = 0, bufferBytes = 0, pendingImageBytes = 0;
        std::uint64_t pendingBufferBytes = 0;
        for (const auto& info : collect.requirements.imageInfos)
        {
            const auto* image = info->imageView->image.get();
            if (!images.insert(image).second || !image->data) continue;
            if (imageData.insert(image->data.get()).second) imageBytes += image->data->dataSize();
            if (!image->getDeviceMemory(device.deviceID) && pendingImageData.insert(image->data.get()).second)
                pendingImageBytes += image->data->dataSize();
        }
        for (const auto& [properties, infos] : collect.requirements.bufferInfos)
            for (const auto& info : infos)
                if (buffers.insert(info.get()).second && info->data)
                {
                    if (bufferData.insert(info->data.get()).second) bufferBytes += info->data->dataSize();
                    if (!info->buffer && pendingBufferData.insert(info->data.get()).second)
                        pendingBufferBytes += info->data->dataSize();
                }
        std::ostringstream out;
        out << "images=" << images.size() << " unique_image_payloads=" << imageData.size()
            << " image_payload_bytes=" << imageBytes << " pending_image_payload_bytes=" << pendingImageBytes
            << " buffers=" << buffers.size() << " buffer_payload_bytes=" << bufferBytes
            << " pending_buffer_payload_bytes=" << pendingBufferBytes;
        if (auto pools = device.deviceMemoryBufferPools.ref_ptr())
        {
            const auto used = pools->computeMemoryTotalReserved();
            out << " pool_reserved_bytes=" << used
                << " pool_free_bytes=" << pools->computeMemoryTotalAvailable();
        }
        return out.str();
    }
}
#endif
