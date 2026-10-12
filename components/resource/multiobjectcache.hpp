#ifndef OPENMW_COMPONENTS_MULTIOBJECTCACHE_H
#define OPENMW_COMPONENTS_MULTIOBJECTCACHE_H

#include <cstddef>
#include <map>
#include <mutex>
#include <memory>

#include <osg/Referenced>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "cachestats.hpp"
#include <components/debug/runtimediagnostics.hpp>

namespace osg
{
    class Object;
    class State;
}

namespace Resource
{

    /// @brief Cache for "non reusable" objects.
    class MultiObjectCache : public osg::Referenced
    {
    public:
        MultiObjectCache();
        ~MultiObjectCache() override;
        void removeUnreferencedObjectsInCache(std::size_t keepUnreferenced = 0);
        std::size_t trimUnused(std::size_t maximum, std::size_t maxScan = 4096);

        /** Remove all objects from the cache. */
        void clear();

        void addEntryToObjectCache(VFS::Path::NormalizedView filename, osg::Object* object);

        /** Take an Object from cache. Return nullptr if no object found. */
        osg::ref_ptr<osg::Object> takeFromObjectCache(VFS::Path::NormalizedView fileName);

        /** call releaseGLObjects on all objects attached to the object cache.*/
        void releaseGLObjects(osg::State* state);

        CacheStats getStats() const;
        void reportRuntimeDiagnostics(std::string_view owner, std::size_t limit) const noexcept;
        // Startup-only independent C3; zero disables it. Keep actual live owners.
        void setIdleRetention(std::size_t coldLimit, double protectedSeconds = 30);
        void updateIdleRetention(double referenceTime, std::size_t controlKeep);
        struct IdleStats
        {
            std::size_t observedIdle = 0, coldLimit = 0, retired = 0;
            std::size_t protectedRecent = 0, protectedExternal = 0, demandKeys = 0;
            bool available = false;
        };
        IdleStats idleStats() const;

    protected:
        typedef std::multimap<VFS::Path::Normalized, osg::ref_ptr<osg::Object>, std::less<>> ObjectCacheMap;

        ObjectCacheMap mObjectCache;
        // All erasing operations repair this iterator while holding the mutex.
        ObjectCacheMap::iterator mTrimNext = mObjectCache.end();
        mutable std::mutex mObjectCacheMutex;
        ObjectCacheMap::iterator mExpiryNext = mObjectCache.end();
        std::size_t mExpiryKept = 0;
        std::size_t mGet = 0;
        std::size_t mHit = 0;
        std::size_t mExpired = 0;
        std::size_t mPressureTrimmed = 0;
    private:
        struct IdlePolicy;
        std::unique_ptr<IdlePolicy> mIdlePolicy;
        void eraseIdleState(const osg::Object* object);
        void recordDemand(VFS::Path::NormalizedView name) noexcept;
    };

}

#endif
