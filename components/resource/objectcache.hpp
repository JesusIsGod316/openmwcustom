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

#include <components/debug/hitchtrace.hpp>
#include <components/debug/resourcelifecycle.hpp>

#include "cachestats.hpp"
#include "cachediagnostics.hpp"
#include "speculativebudget.hpp"
#include "cachemaintenance.hpp"

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
    // A ready owner copied under the cache lock. Payload is released before its
    // matching charge; neither an Item address nor a diagnostic token owns it.
    struct ObjectCacheLease
    {
        ObjectCacheLease() = default;
        ObjectCacheLease(const ObjectCacheLease&) = default;
        ObjectCacheLease(ObjectCacheLease&& other) noexcept
            : mCharge(std::move(other.mCharge)), mValue(other.mValue), mLifecycle(other.mLifecycle)
            , mRevision(other.mRevision), mSourceRevision(other.mSourceRevision), mOwnerEstimate(other.mOwnerEstimate)
        { other.mValue = nullptr; }
        ObjectCacheLease& operator=(ObjectCacheLease other) noexcept
        {
            mCharge.swap(other.mCharge);
            mValue.swap(other.mValue);
            std::swap(mLifecycle, other.mLifecycle);
            std::swap(mRevision, other.mRevision);
            std::swap(mSourceRevision, other.mSourceRevision);
            std::swap(mOwnerEstimate, other.mOwnerEstimate);
            return *this; // old payload then old charge retire with other
        }
        SpeculativeBudget::ChargePtr mCharge;
        osg::ref_ptr<osg::Object> mValue;
        Debug::ResourceLifecycle::Token mLifecycle = 0;
        std::uint64_t mRevision = 0;
        std::uint64_t mSourceRevision = 0;
        std::uint64_t mOwnerEstimate = 0;

        explicit operator bool() const noexcept { return mValue.valid() && mRevision != 0; }
        std::uint64_t bytes() const noexcept { return mCharge ? mCharge->bytes() : 0; }
    };

    struct GenericObjectCacheItem
    {
        GenericObjectCacheItem() = default;
        GenericObjectCacheItem(osg::Object* value, double time) : mValue(value), mLastUsage(time) {}
        // Released after mValue: logical ownership is not freed before payload.
        SpeculativeBudget::ChargePtr mCharge;
        osg::ref_ptr<osg::Object> mValue;
        double mLastUsage = 0;
        std::uint64_t mDiagnosticHits = 0;
        // Pressure trimming protects requests since the last bounded sweep.
        // This never changes normal expiry behavior or depends on diagnostics.
        bool mTrimRecentlyUsed = true;
        Debug::ResourceLifecycle::Token mLifecycle = 0;
        std::uint64_t mRevision = 0;
        std::uint64_t mSourceRevision = 0;
        // Immutable type-specific estimate from the source actually parsed.
        // Zero is unknown; it cannot authorize an optional uncharged pin.
        std::uint64_t mOwnerEstimate = 0;
    };

    template <typename KeyType>
    class GenericObjectCache : public osg::Referenced
    {
    public:
        ~GenericObjectCache() override
        {
            if (Debug::ResourceLifecycle::enabled())
                for (auto& value : mItems)
                    retire(value.second, Debug::ResourceLifecycle::Retirement::CacheDestroyed);
        }
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
        void setSpeculativeBudget(SpeculativeBudget* budget) { mBudget = budget; } // startup only
        bool memoryAccountingEnabled() const noexcept { return mBudget != nullptr; }
        SpeculativeBudget* speculativeBudget() const noexcept { return mBudget; }

        // Startup-only typed manager policy. A wrapper may be cache-only while
        // its shared payload has live consumers (notably parsed NIF files).
        void setExternalOwnerTest(bool (*test)(const osg::Object*) noexcept) { mExternalOwnerTest = test; }

        struct ColdCandidate
        {
            KeyType key;
            ObjectCacheLease lease;
        };
        // A bounded cursor never scans an object graph. Detach a cache-only
        // value atomically: its returned lease is now the sole owner. A demand
        // during encoding takes the ordinary loader path, never reads a buffer
        // that a concurrent consumer could mutate under the codec.
        template<class Eligible>
        std::optional<ColdCandidate> detachColdCandidate(double referenceTime, double age,
            Eligible&& eligible, std::size_t scanLimit = 32)
        {
            std::lock_guard lock(mMutex);
            for (std::size_t scanned = 0; scanned < scanLimit; ++scanned)
            {
                if (auto* budget = CacheMaintenanceScope::current(); budget && !budget->scan()) break;
                auto it = mColdCursor ? mItems.upper_bound(*mColdCursor) : mItems.begin();
                if (it == mItems.end()) { mColdCursor.reset(); break; }
                mColdCursor = it->first;
                auto& item = it->second;
                if (!item.mValue || externallyOwned(item) || !item.mSourceRevision
                    || item.mLastUsage == 0 || item.mLastUsage > referenceTime - age)
                    continue;
                if (!eligible(item.mValue.get())) continue;
                if (std::exchange(item.mTrimRecentlyUsed, false)) continue;
                if (auto* budget = CacheMaintenanceScope::current();
                    budget && !budget->release(item.mCharge ? item.mCharge->bytes() : 0)) break;
                ColdCandidate result{it->first, {}};
                result.lease.mCharge = item.mCharge;
                result.lease.mValue = item.mValue;
                result.lease.mLifecycle = item.mLifecycle;
                result.lease.mRevision = item.mRevision;
                result.lease.mSourceRevision = item.mSourceRevision;
                result.lease.mOwnerEstimate = item.mOwnerEstimate;
                retire(item, Debug::ResourceLifecycle::Retirement::Removed);
                // The lease protects the sole payload and charge; this is not
                // a physical reclamation receipt.
                mItems.erase(it);
                return result;
            }
            return {};
        }

        // Publish a compressed replacement only if demand has not installed a
        // canonical winner meanwhile. The detached original stays alive until
        // this completes (or is republished on codec refusal/failure).
        bool publishDetachedCold(const auto& key, const ObjectCacheLease& expected,
            osg::Object* replacement, std::uint64_t backingBytes, SpeculativeBudget::Stage* reservation = nullptr)
        {
            if (!replacement || !expected || !backingBytes) return false;
            Item incoming(replacement, 0);
            incoming.mSourceRevision = expected.mSourceRevision;
            incoming.mOwnerEstimate = backingBytes;
            if (mBudget)
                incoming.mCharge = mBudget->track({replacement, 3u}, backingBytes, true, reservation);
            {
                std::lock_guard lock(mMutex);
                if (mItems.contains(key) || expected.mValue->referenceCount() != 1) return false;
                auto it = mItems.emplace(key, std::move(incoming)).first;
                publish(it->second);
            }
            return true;
        }

        // Restore/load races publish one canonical winner. The expected cold
        // value is immutable and remains alive for concurrent readers; replacing
        // its cache reference never frees those readers' backing.
        std::pair<osg::ref_ptr<osg::Object>, bool> replaceIfSameOrInsert(const auto& key,
            const osg::Object* expected, osg::Object* object, std::uint64_t sourceRevision)
        {
            Item incoming(object, 0);
            incoming.mSourceRevision = sourceRevision;
            if (mBudget && object)
            {
                const auto* image = dynamic_cast<const osg::Image*>(object);
                const auto* identity = image && image->data() ? static_cast<const void*>(image->data()) : object;
                incoming.mCharge = SpeculativeScope::track(mBudget, {identity, image ? 1u : 2u},
                    image ? image->getTotalSizeInBytesIncludingMipmaps() : 0, image != nullptr);
            }
            Item displaced;
            osg::ref_ptr<osg::Object> result;
            bool inserted = false;
            {
                std::lock_guard lock(mMutex);
                auto it = mItems.find(key);
                if (it != mItems.end() && it->second.mValue.get() != expected)
                    result = it->second.mValue;
                else
                {
                    if (it == mItems.end()) it = mItems.emplace(key, std::move(incoming)).first;
                    else
                    {
                        displaced = std::move(it->second);
                        retire(displaced, Debug::ResourceLifecycle::Retirement::Replaced);
                        it->second = std::move(incoming);
                    }
                    publish(it->second); result = it->second.mValue; inserted = true;
                }
            }
            return {std::move(result), inserted};
        }

        bool removeIfSame(const auto& key, const ObjectCacheLease& expected)
        {
            typename decltype(mItems)::node_type release;
            {
                std::lock_guard lock(mMutex);
                const auto it = mItems.find(key);
                if (it == mItems.end() || it->second.mValue != expected.mValue
                    || it->second.mRevision != expected.mRevision) return false;
                retire(it->second, Debug::ResourceLifecycle::Retirement::Removed);
                release = mItems.extract(it);
            }
            return true;
        }

        // The parsed-source loading candidate alone opts into preserving its
        // actual shared consumers even when expiryDelay is exactly zero.
        // Default behavior and cache-only expiry timestamps remain unchanged.
        void update(double referenceTime, double expiryDelay, bool preserveExternalOwners = false)
        {
            Debug::HitchTrace::Scope scan("resource.cache_expiry_scan", Debug::HitchTrace::CpuTiming::WallEnvelope);
            if (auto* budget = CacheMaintenanceScope::current())
            { maintain(referenceTime, expiryDelay, false, *budget, 128, 8, preserveExternalOwners); return; }
            std::vector<Item> objectsToRemove;
            {
                const double expiryTime = referenceTime - expiryDelay;
                std::lock_guard<std::mutex> lock(mMutex);
                scan.setMetric(0, "entries", mItems.size());

                std::erase_if(mItems, [&](auto& v) {
                    Item& item = v.second;

                    // update last usage timestamp if item is being referenced externally
                    // or initialize if not set
                    const bool external = externallyOwned(item);
                    if (external || item.mLastUsage == 0)
                        item.mLastUsage = referenceTime;

                    // skip items that have been accessed since expiryTime
                    if (item.mLastUsage > expiryTime || (preserveExternalOwners && external))
                        return false;

                    ++mExpired;
                    if (Debug::RuntimeDiagnostics::enabled() && item.mDiagnosticHits == 0)
                        ++mDiagnosticExpiredWithoutHit;

                    retire(item, Debug::ResourceLifecycle::Retirement::Expired);

                    // just mark for removal here so objects can be removed in bulk outside the lock
                    if (item.mValue != nullptr)
                        objectsToRemove.push_back(std::move(item));

                    return true;
                });
            }
            // remove expired items from cache
            { Debug::HitchTrace::Scope release("resource.cache_final_release");
              release.setMetric(0, "owners", objectsToRemove.size()); objectsToRemove.clear(); }
        }

        // Release only cache-owned objects; never strip an active object's
        // image/mesh storage. Work and removal counts are bounded, with a cursor
        // so pinned early keys cannot starve later entries. Destruction is out
        // of the lock and called on the existing resource-maintenance worker.
        std::size_t trimUnused(std::size_t maxRemove, std::size_t maxScan = 4096)
        {
            Debug::HitchTrace::Scope scan("resource.cache_pressure_scan", Debug::HitchTrace::CpuTiming::WallEnvelope);
            if (maxRemove == 0 || maxScan == 0) return 0;
            if (auto* budget = CacheMaintenanceScope::current())
                return maintain(0, 0, true, *budget, maxScan, maxRemove);
            std::vector<Item> release;
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
                    if (!recent && item.mValue && !externallyOwned(item))
                    {
                        const bool neverHit = item.mDiagnosticHits == 0;
                        retire(item, Debug::ResourceLifecycle::Retirement::Pressure);
                        release.push_back(std::move(item));
                        it = mItems.erase(it);
                        ++mPressureTrimmed;
                        if (Debug::RuntimeDiagnostics::enabled() && neverHit)
                            ++mDiagnosticPressureWithoutHit;
                    }
                    else ++it;
                }
                if (it == mItems.end()) mTrimCursor.reset();
            }
            const auto removed = release.size();
            { Debug::HitchTrace::Scope finalRelease("resource.cache_final_release");
              finalRelease.setMetric(0, "owners", removed); release.clear(); }
            scan.setMetric(0, "removed", removed);
            return removed;
        }

        /** Remove all objects in the cache regardless of having external references or expiry times.*/
        void clear()
        {
            decltype(mItems) release;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (Debug::ResourceLifecycle::enabled())
                    for (auto& value : mItems)
                        retire(value.second, Debug::ResourceLifecycle::Retirement::Cleared);
                release.swap(mItems);
                mTrimCursor.reset(); mExpiryCursor.reset(); mColdCursor.reset();
            } // final releases happen outside the cache mutex
            { Debug::HitchTrace::Scope finalRelease("resource.cache_final_release");
              finalRelease.setMetric(0, "owners", release.size()); release.clear(); }
        }

        /** Add a key,object,timestamp triple to the Registry::ObjectCache.*/
        template <class K>
        void addEntryToObjectCache(K&& key, osg::Object* object, double timestamp = 0.0,
            std::uint64_t sourceRevision = 0, std::uint64_t ownerEstimate = 0)
        {
            Item incoming(object, timestamp);
            incoming.mSourceRevision = sourceRevision;
            incoming.mOwnerEstimate = ownerEstimate;
            if (mBudget && object)
            {
                const auto* image = dynamic_cast<const osg::Image*>(object);
                const auto* identity = image && image->data() ? static_cast<const void*>(image->data()) : object;
                incoming.mCharge = SpeculativeScope::track(mBudget, {identity, image ? 1u : 2u},
                    image ? image->getTotalSizeInBytesIncludingMipmaps() : 0, image != nullptr);
            }
            // Incoming and displaced values outlive the lock, including when
            // insertion throws. Never invoke a reentrant destructor under it.
            Item displaced;
            {
                std::unique_lock<std::mutex> lock(mMutex, std::defer_lock);
                { Debug::HitchTrace::Scope wait("cache.insert_lock_wait", Debug::HitchTrace::CpuTiming::Scoped); lock.lock(); }
                Debug::HitchTrace::Scope insert("cache.insert_locked");
                const auto it = mItems.find(key);
                if (it == mItems.end())
                {
                    auto inserted = mItems.emplace_hint(it, std::forward<K>(key), std::move(incoming));
                    publish(inserted->second);
                }
                else
                {
                    displaced = std::move(it->second);
                    retire(displaced, Debug::ResourceLifecycle::Retirement::Replaced);
                    it->second = std::move(incoming);
                    publish(it->second);
                }
            }
        }

        // Publish one canonical resource for concurrent producers. Unlike addEntry,
        // never replaces a winner another chunk may already be using. Construction
        // and speculative charging happen outside the cache lock; a losing object
        // (including its final release) also outlives that lock.
        template <class K>
        std::pair<osg::ref_ptr<osg::Object>, bool> getOrInsert(K&& key, osg::Object* object,
            double timestamp = 0.0)
        {
            Item incoming(object, timestamp);
            if (mBudget && object)
            {
                const auto* image = dynamic_cast<const osg::Image*>(object);
                const auto* identity = image && image->data() ? static_cast<const void*>(image->data()) : object;
                incoming.mCharge = SpeculativeScope::track(mBudget, {identity, image ? 1u : 2u},
                    image ? image->getTotalSizeInBytesIncludingMipmaps() : 0, image != nullptr);
            }
            osg::ref_ptr<osg::Object> result;
            bool inserted = false;
            {
                std::lock_guard lock(mMutex);
                if (Item* existing = find(key))
                    result = existing->mValue;
                else
                {
                    auto it = mItems.emplace(std::forward<K>(key), std::move(incoming)).first;
                    publish(it->second);
                    result = it->second.mValue;
                    inserted = true;
                }
            }
            return {std::move(result), inserted};
        }

        /** Remove Object from cache.*/
        void removeFromObjectCache(const auto& key)
        {
            typename decltype(mItems)::node_type release;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                const auto it = mItems.find(key);
                if (it != mItems.end())
                {
                    retire(it->second, Debug::ResourceLifecycle::Retirement::Removed);
                    release = mItems.extract(it);
                }
            }
        }

        /** Get an ref_ptr<Object> from the object cache*/
        osg::ref_ptr<osg::Object> getRefFromObjectCache(const auto& key)
        {
            std::unique_lock<std::mutex> lock(mMutex, std::defer_lock);
            { Debug::HitchTrace::Scope wait("cache.lookup_lock_wait", Debug::HitchTrace::CpuTiming::Scoped); lock.lock(); }
            Debug::HitchTrace::Scope lookup("cache.lookup_locked");
            if (Item* const item = find(key))
                return item->mValue;
            return nullptr;
        }

        // expectedValue/revision are optional checks for a load followed by
        // adoption. A replacement racing that load must not pair its old value
        // with the new entry's charge/revision. Final releases outlive the lock.
        ObjectCacheLease getLeaseFromObjectCache(const auto& key,
            const osg::Object* expectedValue = nullptr, std::uint64_t sourceRevision = 0)
        {
            ObjectCacheLease result;
            {
                std::lock_guard lock(mMutex);
                if (Item* item = find(key); item && (!expectedValue || item->mValue == expectedValue)
                    && (!sourceRevision || item->mSourceRevision == sourceRevision))
                {
                    result.mCharge = item->mCharge;
                    result.mValue = item->mValue;
                    result.mLifecycle = item->mLifecycle;
                    result.mRevision = item->mRevision;
                    result.mSourceRevision = item->mSourceRevision;
                    result.mOwnerEstimate = item->mOwnerEstimate;
                }
            }
            return result;
        }

        // Reservation and immutable size discovery happen BEFORE this lock.
        // Publish the charge on the exact payload/revision/source snapshot,
        // never by path alone. The canonical pointer is promoted in place, so
        // existing aliases continue owning the same accounted payload.
        bool promoteReadyLease(const auto& key, const ObjectCacheLease& expected,
            std::uint64_t estimate, SpeculativeBudget::Stage& reservation)
        {
            if (!mBudget || !expected || !expected.mCharge || !expected.mSourceRevision)
                return false;
            std::lock_guard lock(mMutex);
            const Item* item = find(key);
            return item && item->mValue == expected.mValue && item->mCharge == expected.mCharge
                && item->mRevision == expected.mRevision && item->mSourceRevision == expected.mSourceRevision
                && item->mLifecycle == expected.mLifecycle
                && item->mOwnerEstimate == expected.mOwnerEstimate && item->mOwnerEstimate == estimate
                && mBudget->promoteUnknown(item->mCharge, estimate, reservation);
        }

        std::optional<osg::ref_ptr<osg::Object>> getRefFromObjectCacheOrNone(const auto& key)
        {
            std::unique_lock<std::mutex> lock(mMutex, std::defer_lock);
            { Debug::HitchTrace::Scope wait("cache.lookup_lock_wait", Debug::HitchTrace::CpuTiming::Scoped); lock.lock(); }
            Debug::HitchTrace::Scope lookup("cache.lookup_locked");
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

    private:
        void publish(GenericObjectCacheItem& item) noexcept
        {
            // Saturation disables new leases without breaking required loading.
            item.mRevision = mLeaseRevision != UINT64_MAX ? ++mLeaseRevision : 0;
            item.mLifecycle = Debug::ResourceLifecycle::publish(this, item.mValue.get());
        }
        static void retire(GenericObjectCacheItem& item, Debug::ResourceLifecycle::Retirement reason) noexcept
        {
            if (item.mLifecycle && item.mValue)
                Debug::ResourceLifecycle::retire(item.mLifecycle, item.mValue.get(), reason,
                    item.mValue->referenceCount(), item.mCharge ? item.mCharge->bytes() : 0);
        }

        std::size_t maintain(double referenceTime, double expiryDelay, bool pressure,
            CacheMaintenanceBudget& budget, std::size_t scanLimit = 128, std::size_t removeLimit = 8,
            bool preserveExternalOwners = false)
        {
            Debug::HitchTrace::Scope scan("resource.cache_bounded_scan", Debug::HitchTrace::CpuTiming::WallEnvelope);
            scanLimit = (std::min)(scanLimit, std::size_t(128));
            removeLimit = (std::min)(removeLimit, std::size_t(8));
            std::size_t scanned = 0, removed = 0;
            auto& cursor = pressure ? mTrimCursor : mExpiryCursor;
            while (scanned < scanLimit && removed < removeLimit && budget.available())
            {
                // One final release between time checks. The map node, value
                // and charge are destroyed outside its mutex.
                typename decltype(mItems)::node_type release;
                bool reachedEnd = false;
                {
                    std::lock_guard lock(mMutex);
                    auto it = cursor ? mItems.upper_bound(*cursor) : mItems.begin();
                    if (it == mItems.end()) { cursor.reset(); break; }
                    if (!budget.scan()) break;
                    ++scanned; cursor = it->first;
                    auto& item = it->second;
                    bool eligible = false;
                    if (pressure)
                    {
                        const bool recent = std::exchange(item.mTrimRecentlyUsed, false);
                        eligible = !recent && item.mValue && !externallyOwned(item);
                    }
                    else
                    {
                        const bool external = externallyOwned(item);
                        if (external || item.mLastUsage == 0)
                            item.mLastUsage = referenceTime;
                        eligible = item.mLastUsage <= referenceTime - expiryDelay
                            && !(preserveExternalOwners && external);
                    }
                    reachedEnd = std::next(it) == mItems.end();
                    if (eligible && budget.release(item.mCharge ? item.mCharge->bytes() : 0))
                    {
                        if (pressure)
                        {
                            ++mPressureTrimmed;
                            if (Debug::RuntimeDiagnostics::enabled() && item.mDiagnosticHits == 0)
                                ++mDiagnosticPressureWithoutHit;
                        }
                        else
                        {
                            ++mExpired;
                            if (Debug::RuntimeDiagnostics::enabled() && item.mDiagnosticHits == 0)
                                ++mDiagnosticExpiredWithoutHit;
                        }
                        retire(item, pressure ? Debug::ResourceLifecycle::Retirement::Pressure
                                              : Debug::ResourceLifecycle::Retirement::Expired);
                        release = mItems.extract(it); ++removed;
                    }
                    if (reachedEnd) cursor.reset();
                }
                if (release && release.mapped().mCharge && release.mapped().mCharge.use_count() == 1)
                    release.mapped().mCharge->pending();
                if (release)
                {
                    Debug::HitchTrace::Scope finalRelease("resource.cache_final_release");
                    finalRelease.setMetric(0, "owners", 1);
                    release = {};
                }
                if (reachedEnd) break;
            }
            scan.setMetric(0, "scanned", scanned); scan.setMetric(1, "removed", removed);
            scan.setMetric(2, "pressure", pressure);
            return removed;
        }

    protected:
        using Item = GenericObjectCacheItem;

        std::map<KeyType, Item, std::less<>> mItems;
        mutable std::mutex mMutex;
        std::optional<KeyType> mTrimCursor, mExpiryCursor, mColdCursor;
        bool (*mExternalOwnerTest)(const osg::Object*) noexcept = nullptr;
        SpeculativeBudget* mBudget = nullptr; // ResourceSystem outlives this cache
        std::size_t mGet = 0;
        std::size_t mHit = 0;
        std::size_t mExpired = 0;
        std::uint64_t mPressureTrimmed = 0, mDiagnosticPressureWithoutHit = 0;
        std::uint64_t mDiagnosticExpiredWithoutHit = 0;
        std::uint64_t mLeaseRevision = 0; // guarded by mMutex

        bool externallyOwned(const Item& item) const noexcept
        {
            return item.mValue && (item.mValue->referenceCount() > 1
                || (mExternalOwnerTest && mExternalOwnerTest(item.mValue.get())));
        }

        Item* find(const auto& key)
        {
            ++mGet;
            const auto it = mItems.find(key);
            if (it == mItems.end())
                return nullptr;
            ++mHit;
            if (mBudget) mBudget->hit(SpeculativeScope::active());
            it->second.mTrimRecentlyUsed = true;
            if (Debug::RuntimeDiagnostics::enabled()) ++it->second.mDiagnosticHits;
            Debug::ResourceLifecycle::observe(it->second.mLifecycle, this, it->second.mValue.get());
            return &it->second;
        }
    };
}

#endif
