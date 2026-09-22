#include "multiobjectcache.hpp"

#include <vector>

#include <osg/Object>

namespace Resource
{
    void MultiObjectCache::removeUnreferencedObjectsInCache(std::size_t keepUnreferenced)
    {
        std::vector<osg::ref_ptr<osg::Object>> objectsToRemove;
        {
            std::lock_guard<std::mutex> lock(mObjectCacheMutex);

            // Keep a bounded pool of unused instances for reuse. This preserves
            // upstream behavior when keepUnreferenced == 0.
            std::size_t kept = 0;
            ObjectCacheMap::iterator oitr = mObjectCache.begin();
            while (oitr != mObjectCache.end())
            {
                if (oitr->second->referenceCount() <= 1)
                {
                    if (kept < keepUnreferenced)
                    {
                        ++kept;
                        ++oitr;
                        continue;
                    }
                    objectsToRemove.push_back(oitr->second);
                    mObjectCache.erase(oitr++);
                    ++mExpired;
                }
                else
                {
                    ++oitr;
                }
            }
        }

        // note, actual unref happens outside of the lock
        objectsToRemove.clear();
    }

    void MultiObjectCache::clear()
    {
        std::lock_guard<std::mutex> lock(mObjectCacheMutex);
        mObjectCache.clear();
    }

    void MultiObjectCache::addEntryToObjectCache(VFS::Path::NormalizedView filename, osg::Object* object)
    {
        if (!object)
        {
            OSG_ALWAYS << " trying to add NULL object to cache for " << filename << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(mObjectCacheMutex);
        mObjectCache.emplace(filename, object);
    }

    osg::ref_ptr<osg::Object> MultiObjectCache::takeFromObjectCache(VFS::Path::NormalizedView fileName)
    {
        std::lock_guard<std::mutex> lock(mObjectCacheMutex);
        ++mGet;
        const auto it = mObjectCache.find(fileName);
        if (it != mObjectCache.end())
        {
            osg::ref_ptr<osg::Object> object = std::move(it->second);
            mObjectCache.erase(it);
            ++mHit;
            return object;
        }

        return nullptr;
    }

    void MultiObjectCache::releaseGLObjects(osg::State* state)
    {
        std::lock_guard<std::mutex> lock(mObjectCacheMutex);

        for (ObjectCacheMap::iterator itr = mObjectCache.begin(); itr != mObjectCache.end(); ++itr)
        {
            osg::Object* object = itr->second.get();
            object->releaseGLObjects(state);
        }
    }

    void MultiObjectCache::reportRuntimeDiagnostics(std::string_view owner, std::size_t limit) const noexcept
    {
        if (!Debug::RuntimeDiagnostics::enabled()) return;
        try
        {
            std::unique_lock lock(mObjectCacheMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                Debug::RuntimeDiagnostics::recordEvent("coverage", owner, "pool lock busy", {{"available", 0}});
                return;
            }
            std::uint64_t external = 0;
            for (const auto& [key, value] : mObjectCache)
                if (value && value->referenceCount() > 1) ++external;
            Debug::RuntimeDiagnostics::recordEvent("cache_pool", owner, "Object counts; payload bytes unmeasured", {
                {"entries", mObjectCache.size()}, {"lookups", mGet}, {"hits", mHit}, {"expired", mExpired},
                {"external_refs", external}, {"keep_unreferenced_limit", limit}, {"payload_measured", 0}});
        }
        catch (...) { Debug::RuntimeDiagnostics::recordEvent("coverage", owner, "pool census unavailable", {{"available", 0}}); }
    }

    CacheStats MultiObjectCache::getStats() const
    {
        std::lock_guard<std::mutex> lock(mObjectCacheMutex);
        return CacheStats{
            .mSize = mObjectCache.size(),
            .mGet = mGet,
            .mHit = mHit,
            .mExpired = mExpired,
        };
    }

}
