// Resource ObjectCache for OpenMW, forked from osgDB ObjectCache by Robert Osfield, see copyright notice below.
// Changes:
// - removeExpiredObjectsInCache no longer keeps a lock while the unref happens.
// - template allows customized KeyType.
// - objects with uninitialized time stamp are not removed.

/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
 */

#ifndef OPENMW_COMPONENTS_RESOURCE_OBJECTCACHE
#define OPENMW_COMPONENTS_RESOURCE_OBJECTCACHE

#include "cachestats.hpp"
#include "cachediagnostics.hpp"

#include <osg/Node>
#include <osg/Referenced>
#include <osg/ref_ptr>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace osg
{
    class Object;
    class State;
    class NodeVisitor;
    class Stats;
}

namespace Resource
{
    struct GenericObjectCacheItem
    {
        osg::ref_ptr<osg::Object> mValue;
        double mLastUsage;
        std::uint64_t mDiagnosticHits = 0;
        // Pressure trimming protects requests since the last bounded sweep.
        // This never changes normal expiry behavior or depends on diagnostics.
        bool mTrimRecentlyUsed = true;
    };

    template <typename KeyType>
    class GenericObjectCache : public osg::Referenced
    {
    public:
        /*
         * @brief Updates usage timestamps and removes expired items
         *
         * Updates the lastUsage timestamp of cached non-nullptr items that have external references.
         * Initializes lastUsage timestamp for new items.
         * Removes items that haven't been referenced for longer than expiryDelay.
         *
         * \note
         * Last usage might be updated from other places so nullptr items
         * that are not referenced elsewhere are not always removed.
         *
         * @param referenceTime the timestamp indicating when the item was most recently used
         * @param expiryDelay the delay after which the cache entry for an item expires
         */
        void update(double referenceTime, double expiryDelay)
        {
            std::vector<osg::ref_ptr<osg::Object>> objectsToRemove;
            {
                const double expiryTime = referenceTime - expiryDelay;
                std::lock_guard<std::mutex> lock(mMutex);

                std::erase_if(mItems, [&](auto& v) {
                    Item& item = v.second;

                    // update last usage timestamp if item is being referenced externally
                    // or initialize if not set
                    if ((item.mValue != nullptr && item.mValue->referenceCount() > 1) || item.mLastUsage == 0)
                        item.mLastUsage = referenceTime;

                    // skip items that have been accessed since expiryTime
                    if (item.mLastUsage > expiryTime)
                        return false;

                    ++mExpired;
                    if (Debug::RuntimeDiagnostics::enabled() && item.mDiagnosticHits == 0)
                        ++mDiagnosticExpiredWithoutHit;

                    // just mark for removal here so objects can be removed in bulk outside the lock
                    if (item.mValue != nullptr)
                        objectsToRemove.push_back(std::move(item.mValue));

                    return true;
                });
            }
            // remove expired items from cache
            objectsToRemove.clear();
        }

        // Release only cache-owned objects; never strip an active object's
        // image/mesh storage. Work and removal counts are bounded, with a cursor
        // so pinned early keys cannot starve later entries. Destruction is out
        // of the lock and called on the existing resource-maintenance worker.
        std::size_t trimUnused(std::size_t maxRemove, std::size_t maxScan = 4096)
        {
            if (maxRemove == 0 || maxScan == 0) return 0;
            std::vector<osg::ref_ptr<osg::Object>> release;
            release.reserve((std::min)(maxRemove, maxScan));
            {
                std::lock_guard lock(mMutex);
                auto it = mTrimCursor ? mItems.upper_bound(*mTrimCursor) : mItems.begin();
                std::size_t scanned = 0;
                while (it != mItems.end() && scanned++ < maxScan && release.size() < maxRemove)
                {
                    mTrimCursor = it->first;
                    Item& item = it->second;
                    const bool recent = std::exchange(item.mTrimRecentlyUsed, false);
                    if (!recent && item.mValue && item.mValue->referenceCount() == 1)
                    {
                        release.push_back(std::move(item.mValue));
                        const bool neverHit = item.mDiagnosticHits == 0;
                        it = mItems.erase(it);
                        ++mPressureTrimmed;
                        if (Debug::RuntimeDiagnostics::enabled() && neverHit)
                            ++mDiagnosticPressureWithoutHit;
                    }
                    else ++it;
                }
                if (it == mItems.end()) mTrimCursor.reset();
            }
            return release.size();
        }

        /** Remove all objects in the cache regardless of having external references or expiry times.*/
        void clear()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mItems.clear();
        }

        /** Add a key,object,timestamp triple to the Registry::ObjectCache.*/
        template <class K>
        void addEntryToObjectCache(K&& key, osg::Object* object, double timestamp = 0.0)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto it = mItems.find(key);
            if (it == mItems.end())
                mItems.emplace_hint(it, std::forward<K>(key), Item{ object, timestamp });
            else
                it->second = Item{ object, timestamp };
        }

        /** Remove Object from cache.*/
        void removeFromObjectCache(const auto& key)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto itr = mItems.find(key);
            if (itr != mItems.end())
                mItems.erase(itr);
        }

        /** Get an ref_ptr<Object> from the object cache*/
        osg::ref_ptr<osg::Object> getRefFromObjectCache(const auto& key)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (Item* const item = find(key))
                return item->mValue;
            return nullptr;
        }

        std::optional<osg::ref_ptr<osg::Object>> getRefFromObjectCacheOrNone(const auto& key)
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            if (Item* const item = find(key))
                return item->mValue;
            return std::nullopt;
        }

        /** Check if an object is in the cache, and if it is, update its usage time stamp. */
        bool checkInObjectCache(const auto& key, double timeStamp)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (Item* const item = find(key))
            {
                item->mLastUsage = timeStamp;
                return true;
            }
            return false;
        }

        /** call releaseGLObjects on all objects attached to the object cache.*/
        void releaseGLObjects(osg::State* state)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                v.mValue->releaseGLObjects(state);
        }

        /** call node->accept(nv); for all nodes in the objectCache. */
        void accept(osg::NodeVisitor& nv)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                if (osg::Object* const object = v.mValue.get())
                    if (osg::Node* const node = dynamic_cast<osg::Node*>(object))
                        node->accept(nv);
        }

        /** call operator()(KeyType, osg::Object*) for each object in the cache. */
        template <class Functor>
        void call(Functor&& f)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                f(k, v.mValue.get());
        }

        template <class Functor>
        void callWithUsage(Functor&& f) const
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                f(k, v.mValue.get(), v.mLastUsage);
        }

        template <class K>
        std::optional<std::pair<KeyType, osg::ref_ptr<osg::Object>>> lowerBound(K&& key)
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            const auto it = mItems.lower_bound(std::forward<K>(key));
            if (it == mItems.end())
                return std::nullopt;
            return std::pair(it->first, it->second.mValue);
        }

        CacheStats getStats() const
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            return CacheStats{
                .mSize = mItems.size(),
                .mGet = mGet,
                .mHit = mHit,
                .mExpired = mExpired,
            };
        }

        void reportRuntimeDiagnostics(std::string_view owner, double referenceTime, double expiryDelay) const noexcept
        {
            if (!Debug::RuntimeDiagnostics::enabled()) return;
            const auto start = Debug::RuntimeDiagnostics::nowUs();
            try
            {
                CacheDiagnosticCensus census;
                CacheStats stats;
                std::uint64_t neverHit = 0, pressureTrimmed = 0, pressureWithoutHit = 0;
                {
                    std::unique_lock lock(mMutex, std::try_to_lock);
                    if (!lock.owns_lock())
                    {
                        Debug::RuntimeDiagnostics::recordEvent("coverage", owner, "cache lock busy", {{"available", 0}});
                        return;
                    }
                    stats = { mItems.size(), mGet, mHit, mExpired };
                    neverHit = mDiagnosticExpiredWithoutHit;
                    pressureTrimmed = mPressureTrimmed;
                    pressureWithoutHit = mDiagnosticPressureWithoutHit;
                    for (const auto& [key, item] : mItems)
                    {
                        if (census.entries >= CacheDiagnosticCensus::Limit) { census.limited = true; break; }
                        std::string_view name;
                        if constexpr (requires { std::string_view(key); }) name = std::string_view(key);
                        else if constexpr (requires { std::string_view(key.value()); }) name = key.value();
                        census.add(name, item.mValue.get(), item.mLastUsage, referenceTime);
                    }
                }
                Debug::RuntimeDiagnostics::recordEvent("cache_pressure", owner, {}, {
                    {"instance", reinterpret_cast<std::uintptr_t>(this)}, {"trimmed", pressureTrimmed},
                    {"trimmed_without_hit", pressureWithoutHit}});
                Debug::RuntimeDiagnostics::recordEvent("cache", owner, {}, {
                    {"instance", reinterpret_cast<std::uintptr_t>(this)}, {"entries", stats.mSize},
                    {"lookups", stats.mGet}, {"hits", stats.mHit}, {"expired", stats.mExpired},
                    {"expired_without_hit", neverHit}, {"sampled_entries", census.entries},
                    {"external_refs", census.externallyReferenced}, {"cache_only_entries", census.cacheOnly},
                    {"known_payload_bytes", census.knownPayloadBytes},
                    {"cache_only_payload_bytes", census.cacheOnlyPayloadBytes},
                    {"unmeasured_entries", census.unmeasuredEntries}, {"shared_payload_refs", census.sharedPayloadReferences},
                    {"oldest_inactive_us", census.oldestInactiveUs}, {"limited", census.limited},
                    {"expiry_us", expiryDelay > 0 ? static_cast<std::uint64_t>(expiryDelay * 1000000.0) : 0} });
                if (Debug::RuntimeDiagnostics::mode() == Debug::RuntimeDiagnostics::Mode::Focused)
                    for (const auto& top : census.top)
                        if (top.bytes) Debug::RuntimeDiagnostics::recordEvent("cache_asset", owner, top.name.data(),
                            {{"known_payload_bytes", top.bytes}, {"external_ref_observed", top.external}});
                Debug::RuntimeDiagnostics::recordEvent("probe_cost", owner, "cache census", {
                    {"elapsed_us", Debug::RuntimeDiagnostics::nowUs() - start}, {"source_elements", census.sourceElements}});
            }
            catch (...) { Debug::RuntimeDiagnostics::recordEvent("coverage", owner, "cache census unavailable", {{"available", 0}}); }
        }

    protected:
        using Item = GenericObjectCacheItem;

        std::map<KeyType, Item, std::less<>> mItems;
        mutable std::mutex mMutex;
        std::optional<KeyType> mTrimCursor;
        std::size_t mGet = 0;
        std::size_t mHit = 0;
        std::size_t mExpired = 0;
        std::uint64_t mPressureTrimmed = 0, mDiagnosticPressureWithoutHit = 0;
        std::uint64_t mDiagnosticExpiredWithoutHit = 0;

        Item* find(const auto& key)
        {
            ++mGet;
            const auto it = mItems.find(key);
            if (it == mItems.end())
                return nullptr;
            ++mHit;
            it->second.mTrimRecentlyUsed = true;
            if (Debug::RuntimeDiagnostics::enabled()) ++it->second.mDiagnosticHits;
            return &it->second;
        }
    };
}

#endif
