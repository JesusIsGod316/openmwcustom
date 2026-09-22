#ifndef OPENMW_RENDER_VSG_LIVETEXTURECACHE_H
#define OPENMW_RENDER_VSG_LIVETEXTURECACHE_H

#include "staticassetrealizer.hpp"
#include <vsg/core/observer_ptr.h>
#include <memory>
#include <map>
#include <mutex>
#include <tuple>

namespace RenderVsg
{
    // Share decoded immutable payloads while scene descriptors still own them.
    // Weak entries cannot keep unused textures resident; the bounded index also
    // cannot grow across an arbitrarily long sequence of cell/world changes.
    inline StaticTextureResolver cacheLiveTextures(StaticTextureResolver decode, std::size_t capacity = 4096)
    {
        // Handles are local to a RenderWorld. Immediate effects use temporary
        // worlds, while static/actor records can give the same content different
        // handles. Share by immutable winning content and interpretation instead.
        using Key = std::tuple<std::string, std::string, std::uint64_t,
            RenderCore::TextureColorSpace, RenderCore::TextureFormatClass>;
        struct Cache
        {
            std::mutex mutex;
            std::map<Key, vsg::observer_ptr<vsg::Data>> entries;
        };
        return [cache = std::make_shared<Cache>(), decode = std::move(decode), capacity](
                   const RenderCore::TextureRecord& record, const RenderCore::TextureRealizationKey& key)
                   -> vsg::ref_ptr<vsg::Data> {
            if (!decode || !key.valid() || record.contentIdentity.empty() || record.revision != key.revision)
                return {};
            if (capacity == 0)
                return decode(record, key);
            const Key identity{ record.sourceIdentity, record.contentIdentity, key.revision.value(),
                key.view.colorSpace, key.view.formatClass };
            std::lock_guard lock(cache->mutex);
            auto found = cache->entries.find(identity);
            if (found != cache->entries.end())
            {
                if (auto live = found->second.ref_ptr())
                    return live;
                cache->entries.erase(found);
            }
            auto data = decode(record, key);
            if (!data) return {};
            if (cache->entries.size() >= capacity)
            {
                std::erase_if(cache->entries, [](const auto& item) { return !item.second.valid(); });
                // Dropping a weak index entry never invalidates a live resource.
                if (cache->entries.size() >= capacity) cache->entries.erase(cache->entries.begin());
            }
            cache->entries.emplace(identity, vsg::observer_ptr<vsg::Data>(data.get()));
            return data;
        };
    }
}
#endif
