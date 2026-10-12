#include "multiobjectcache.hpp"
#include "cachemaintenance.hpp"

#include <algorithm>
#include <utility>
#include <vector>
#include <list>
#include <cmath>
#include <atomic>

#include <osg/Object>

namespace Resource
{
    struct MultiObjectCache::IdlePolicy
    {
        struct Entry
        {
            double lastHeld = 0;
            bool idle = false;
            std::list<const osg::Object*>::iterator position;
            ObjectCacheMap::iterator owner;
        };
        struct Demand
        {
            double time = 0;
            std::list<VFS::Path::Normalized>::iterator position;
        };
        std::size_t limit = 0;
        double protection = 30, time = 0;
        std::map<const osg::Object*, Entry> entries;
        std::list<const osg::Object*> idle;
        std::list<const osg::Object*>::iterator victim = idle.end();
        std::map<VFS::Path::Normalized, Demand, std::less<>> demands;
        std::list<VFS::Path::Normalized> demandLru;
        std::size_t retired = 0, recentProtected = 0, externalProtected = 0;
        std::atomic_bool available{true};
    };

    MultiObjectCache::MultiObjectCache() = default;
    MultiObjectCache::~MultiObjectCache() = default;

    void MultiObjectCache::setIdleRetention(std::size_t limit, double protectedSeconds)
    {
        if (!limit) { mIdlePolicy.reset(); return; }
        mIdlePolicy = std::make_unique<IdlePolicy>();
        mIdlePolicy->limit = limit;
        mIdlePolicy->protection = std::isfinite(protectedSeconds) && protectedSeconds >= 0 ? protectedSeconds : 30;
    }

    void MultiObjectCache::eraseIdleState(const osg::Object* object)
    {
        if (!mIdlePolicy) return;
        const auto it = mIdlePolicy->entries.find(object);
        if (it == mIdlePolicy->entries.end()) return;
        if (it->second.idle)
        {
            if (mIdlePolicy->victim == it->second.position) ++mIdlePolicy->victim;
            mIdlePolicy->idle.erase(it->second.position);
        }
        mIdlePolicy->entries.erase(it);
    }

    void MultiObjectCache::recordDemand(VFS::Path::NormalizedView name) noexcept
    {
        if (!mIdlePolicy || !mIdlePolicy->available) return;
        auto& policy = *mIdlePolicy;
        try
        {
        auto it = policy.demands.find(name);
        if (it != policy.demands.end())
        {
            it->second.time = policy.time;
            policy.demandLru.splice(policy.demandLru.end(), policy.demandLru, it->second.position);
            return;
        }
        if (policy.demands.size() >= 512)
        {
            policy.demands.erase(policy.demandLru.front());
            policy.demandLru.pop_front();
        }
        policy.demandLru.emplace_back(name);
        try
        {
            policy.demands.emplace(policy.demandLru.back(),
                IdlePolicy::Demand{policy.time, std::prev(policy.demandLru.end())});
        }
        catch (...) { policy.demandLru.pop_back(); throw; }
        }
        catch (...) { policy.available = false; } // retain original pool on indexing failure
    }

    void MultiObjectCache::updateIdleRetention(double referenceTime, std::size_t controlKeep)
    {
        if (!mIdlePolicy) { removeUnreferencedObjectsInCache(controlKeep); return; }
        if (!mIdlePolicy->available) { removeUnreferencedObjectsInCache(controlKeep); return; }
        if (!std::isfinite(referenceTime)) return;
        auto* budget = CacheMaintenanceScope::current();
        // Discover ownership transitions with one bounded rotating cursor. The
        // LRU contains raw identities only and never adds an owning reference.
        try
        {
        for (std::size_t scanned = 0; scanned < 96; ++scanned)
        {
            std::lock_guard lock(mObjectCacheMutex);
            auto& policy = *mIdlePolicy;
            policy.time = (std::max)(policy.time, referenceTime);
            if (budget && !budget->scan()) break;
            if (mExpiryNext == mObjectCache.end()) mExpiryNext = mObjectCache.begin();
            if (mExpiryNext == mObjectCache.end()) break;
            auto it = mExpiryNext++;
            auto& state = policy.entries.try_emplace(it->second.get()).first->second;
            state.owner = it;
            if (it->second->referenceCount() > 1)
            {
                if (state.idle)
                {
                    if (policy.victim == state.position) ++policy.victim;
                    policy.idle.erase(state.position); state.idle = false;
                }
                state.lastHeld = policy.time;
            }
            else if (!state.idle)
            {
                // The first observed release begins a short-return grace
                // interval; engine wall time, not a speculative touch count.
                state.lastHeld = policy.time;
                policy.idle.push_back(it->second.get());
                state.position = std::prev(policy.idle.end()); state.idle = true;
            }
            if (mExpiryNext == mObjectCache.end()) break;
        }
        std::size_t removed = 0;
        for (std::size_t scanned = 0; scanned < 32 && removed < 8; ++scanned)
        {
            osg::ref_ptr<osg::Object> released;
            {
                std::lock_guard lock(mObjectCacheMutex);
                auto& policy = *mIdlePolicy;
                if (policy.idle.size() <= policy.limit || (budget && !budget->scan())) break;
                if (policy.victim == policy.idle.end()) policy.victim = policy.idle.begin();
                auto candidate = policy.victim++;
                const auto* identity = *candidate;
                const auto state = policy.entries.find(identity);
                // Each state indexes the exact stable multimap node. No list or
                // object-cache search grows with the full pool population.
                if (state == policy.entries.end())
                { policy.idle.erase(candidate); continue; }
                auto it = state->second.owner;
                if (it->second->referenceCount() > 1)
                {
                    ++policy.externalProtected;
                    policy.idle.erase(candidate); state->second.idle = false; state->second.lastHeld = policy.time;
                    continue;
                }
                const auto demand = policy.demands.find(it->first);
                const bool recent = state->second.lastHeld > policy.time - policy.protection
                    || (demand != policy.demands.end() && demand->second.time > policy.time - policy.protection);
                if (recent) { ++policy.recentProtected; continue; }
                if (budget && !budget->release()) break;
                if (mTrimNext == it) ++mTrimNext;
                if (mExpiryNext == it) ++mExpiryNext;
                eraseIdleState(identity);
                released = std::move(it->second); mObjectCache.erase(it);
                ++removed; ++mExpired; ++policy.retired;
            }
            // Potentially expensive shape destruction occurs outside the lock,
            // on the existing resource-maintenance worker between budget checks.
        }
        }
        catch (...)
        {
            std::lock_guard lock(mObjectCacheMutex);
            mIdlePolicy->available = false;
        }
    }

    MultiObjectCache::IdleStats MultiObjectCache::idleStats() const
    {
        std::lock_guard lock(mObjectCacheMutex);
        if (!mIdlePolicy) return {};
        const auto& p = *mIdlePolicy;
        return {p.idle.size(), p.limit, p.retired, p.recentProtected, p.externalProtected, p.demands.size(), p.available};
    }

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
                            eraseIdleState(it->second.get());
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
                    eraseIdleState(oitr->second.get());
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
                        eraseIdleState(it->second.get());
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
                    eraseIdleState(it->second.get());
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
            if (mIdlePolicy)
            {
                mIdlePolicy->entries.clear(); mIdlePolicy->idle.clear();
                mIdlePolicy->victim = mIdlePolicy->idle.end();
                mIdlePolicy->demands.clear(); mIdlePolicy->demandLru.clear();
            }
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
            recordDemand(fileName);
            eraseIdleState(it->second.get());
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
            if (mIdlePolicy)
                Debug::RuntimeDiagnostics::recordEvent("idle_pool_retention", owner,
                    "Observed idle identities and protected-use counts; payload bytes unmeasured", {
                        {"observed_idle", mIdlePolicy->idle.size()}, {"cold_limit", mIdlePolicy->limit},
                        {"retired", mIdlePolicy->retired}, {"recent_protection_checks", mIdlePolicy->recentProtected},
                        {"external_protection_checks", mIdlePolicy->externalProtected},
                        {"demand_keys", mIdlePolicy->demands.size()}, {"index_available", mIdlePolicy->available}});
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
