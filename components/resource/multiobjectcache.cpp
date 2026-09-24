#include "multiobjectcache.hpp"
#include "cachemaintenance.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <osg/Object>

namespace Resource
{
    void MultiObjectCache::removeUnreferencedObjectsInCache(std::size_t keepUnreferenced)
    {
        if (auto* budget = CacheMaintenanceScope::current())
        {
            for (unsigned scanned = 0; scanned < 128 && budget->available(); ++scanned)
            {
                osg::ref_ptr<osg::Object> released;
                bool end = false;
                {
                    std::lock_guard lock(mObjectCacheMutex);
                    if (mExpiryNext == mObjectCache.end()) { mExpiryNext = mObjectCache.begin(); mExpiryKept = 0; }
                    if (mExpiryNext == mObjectCache.end() || !budget->scan()) break;
                    auto it = mExpiryNext++;
                    if (it->second->referenceCount() <= 1)
                    {
                        if (mExpiryKept < keepUnreferenced) ++mExpiryKept;
                        else if (budget->release())
                        {
                            if (mTrimNext == it) ++mTrimNext;
                            released = std::move(it->second); mObjectCache.erase(it); ++mExpired;
                        }
                    }
                    end = mExpiryNext == mObjectCache.end();
                }
                if (end) break;
            }
            return;
        }
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
                    if (mTrimNext == oitr) ++mTrimNext;
                    if (mExpiryNext == oitr) ++mExpiryNext;
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

    std::size_t MultiObjectCache::trimUnused(std::size_t maximum, std::size_t maxScan)
    {
        if (maximum == 0 || maxScan == 0) return 0;
        if (auto* budget = CacheMaintenanceScope::current())
        {
            std::size_t removed = 0;
            for (std::size_t scanned = 0; scanned < (std::min)(maxScan, std::size_t(128))
                && removed < maximum && budget->available(); ++scanned)
            {
                osg::ref_ptr<osg::Object> released;
                bool end = false;
                {
                    std::lock_guard lock(mObjectCacheMutex);
                    if (mTrimNext == mObjectCache.end()) mTrimNext = mObjectCache.begin();
                    if (mTrimNext == mObjectCache.end() || !budget->scan()) break;
                    auto it = mTrimNext++;
                    if (it->second && it->second->referenceCount() == 1 && budget->release())
                    {
                        if (mExpiryNext == it) ++mExpiryNext;
                        released = std::move(it->second); mObjectCache.erase(it); ++removed; ++mPressureTrimmed;
                    }
                    end = mTrimNext == mObjectCache.end();
                }
                if (end) break;
            }
            return removed;
        }
        std::vector<osg::ref_ptr<osg::Object>> release;
        release.reserve((std::min)(maximum, maxScan));
        {
            std::lock_guard lock(mObjectCacheMutex);
            if (mTrimNext == mObjectCache.end()) mTrimNext = mObjectCache.begin();
            std::size_t scanned = 0;
            while (mTrimNext != mObjectCache.end() && scanned++ < maxScan && release.size() < maximum)
            {
                auto it = mTrimNext++;
                if (it->second && it->second->referenceCount() == 1)
                {
                    release.push_back(std::move(it->second));
                    if (mExpiryNext == it) ++mExpiryNext;
                    mObjectCache.erase(it);
                    ++mPressureTrimmed;
                }
            }
        }
        return release.size();
    }

    void MultiObjectCache::clear()
    {
        ObjectCacheMap released;
        {
            std::lock_guard<std::mutex> lock(mObjectCacheMutex);
            released.swap(mObjectCache);
            mTrimNext = mObjectCache.end(); mExpiryNext = mObjectCache.end(); mExpiryKept = 0;
        }
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
            if (mTrimNext == it) ++mTrimNext;
            if (mExpiryNext == it) ++mExpiryNext;
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
                {"pressure_trimmed", mPressureTrimmed}, {"external_refs", external}, {"keep_unreferenced_limit", limit}, {"payload_measured", 0}});
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
