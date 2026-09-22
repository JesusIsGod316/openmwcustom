#ifndef OPENMW_RENDER_VSG_LIVETEXTUREIMAGES_H
#define OPENMW_RENDER_VSG_LIVETEXTUREIMAGES_H

#include <vsg/core/observer_ptr.h>
#include <vsg/state/ImageInfo.h>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace RenderVsg
{
    // The decode cache alone cannot share GPU memory: ImageInfo(sampler, data)
    // creates a new Image each time. Keep weak references to live immutable
    // sampled images across independent configurators and frame generations.
    // Sampler::compare includes the entire sampling contract (including maxLod,
    // which determines image mip allocation). Dynamic images never enter here.
    class LiveTextureImages
    {
    public:
        explicit LiveTextureImages(std::size_t capacity = 8192) : mCapacity(capacity) {}

        vsg::ref_ptr<vsg::ImageInfo> get(vsg::ref_ptr<vsg::Data> data, vsg::ref_ptr<vsg::Sampler> sampler)
        {
            if (!data || !sampler) return {};
            if (!mCapacity || data->dynamic()) return vsg::ImageInfo::create(sampler, data);
            std::lock_guard lock(mMutex);
            auto found = mEntries.find(data.get());
            if (found != mEntries.end())
            {
                for (auto& weak : found->second)
                    if (auto live = weak.ref_ptr(); live && live->sampler->compare(*sampler) == 0)
                        return live;
            }
            // Count entries, not just data pointers: arbitrarily many sampler
            // variants of a single texture must not grow this index indefinitely.
            if (mSize >= mCapacity)
            {
                for (auto it = mEntries.begin(); it != mEntries.end();)
                {
                    mSize -= std::erase_if(it->second, [](const auto& weak) { return !weak.valid(); });
                    if (it->second.empty()) it = mEntries.erase(it);
                    else ++it;
                }
                if (mSize >= mCapacity)
                {
                    auto it = mEntries.begin();
                    mSize -= it->second.size();
                    mEntries.erase(it); // index only; scene/fence owners remain
                }
            }
            auto image = vsg::ImageInfo::create(sampler, data);
            mEntries[data.get()].emplace_back(image.get());
            ++mSize;
            return image;
        }

    private:
        const std::size_t mCapacity;
        std::size_t mSize = 0;
        std::mutex mMutex;
        std::unordered_map<const vsg::Data*, std::vector<vsg::observer_ptr<vsg::ImageInfo>>> mEntries;
    };

    inline vsg::ref_ptr<vsg::ImageInfo> liveTextureImage(
        vsg::ref_ptr<vsg::Data> data, vsg::ref_ptr<vsg::Sampler> sampler)
    {
        static LiveTextureImages images;
        if (std::getenv("OPENMW_V4_UNSHARED_TEXTURE_IMAGES"))
            return vsg::ImageInfo::create(sampler, data);
        return images.get(std::move(data), std::move(sampler));
    }
}
#endif
