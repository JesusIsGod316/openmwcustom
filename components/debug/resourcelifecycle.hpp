#ifndef OPENMW_COMPONENTS_DEBUG_RESOURCELIFECYCLE_H
#define OPENMW_COMPONENTS_DEBUG_RESOURCELIFECYCLE_H

#include "hitchtrace.hpp"

#include <components/vfs/manager.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace Debug::ResourceLifecycle
{
    using Token = std::uint64_t;
    enum class Kind { Template, Nif, Keyframe, PreparedInstance };
    enum class Retirement : std::uint64_t
    { Expired = 1, Pressure = 2, Replaced = 3, Removed = 4, Cleared = 5, CacheDestroyed = 6, PoolConsumed = 7 };
    inline constexpr std::uint64_t UnknownGeneration = 4;
    inline constexpr std::uint64_t PreparationObserved = 16;
    inline constexpr std::size_t Capacity = 65536;

    inline bool enabled() noexcept
    { return HitchTrace::requested() && HitchTrace::Capture::instance().enabled(); }
    inline std::string_view name(Kind kind) noexcept
    {
        switch (kind)
        {
            case Kind::Template: return "template";
            case Kind::Nif: return "nif";
            case Kind::Keyframe: return "keyframe";
            case Kind::PreparedInstance: return "prepared_instance";
        }
        return "unknown";
    }

    // Only an explicit optional-worker boundary establishes preparation origin.
    // It owns no resource and cannot change resource/cache scheduling.
    class PreparationScope;
    inline thread_local PreparationScope* preparation = nullptr;
    class PreparationScope
    {
    public:
        PreparationScope(std::string_view identity, std::uint64_t originatingSpan = 0) noexcept
        {
            if (!enabled()) return;
            mPrevious = preparation;
            mScope.emplace("resource.preparation_origin", HitchTrace::CpuTiming::WallEnvelope, identity);
            mScope->retain(); mScope->setLink(originatingSpan);
            preparation = this;
        }
        ~PreparationScope() { if (mScope) preparation = mPrevious; }
        std::uint64_t id() const noexcept { return mScope ? mScope->id() : 0; }
    private:
        PreparationScope* mPrevious = nullptr;
        std::optional<HitchTrace::Scope> mScope;
    };

    struct Context
    {
        Kind kind = Kind::Template;
        std::string_view asset;
        const VFS::Manager* vfs = nullptr;
        std::uint64_t generation = 0, preparationSpan = 0;
        // 0 unclassified speculation, 1 required API retrieval, 2 declared preparation.
        std::uint64_t origin = 0;
        const void* cacheOwner = nullptr;
    };
    inline thread_local const Context* request = nullptr;
    class RequestScope
    {
    public:
        RequestScope(Kind kind, std::string_view asset, const VFS::Manager* vfs, bool speculative,
            const void* cacheOwner) noexcept
        {
            if (!enabled()) return;
            mPrevious = request;
            mContext = {kind, asset, vfs, vfs ? vfs->getIndexGeneration() : 0,
                preparation ? preparation->id() : 0, preparation ? 2u : speculative ? 0u : 1u, cacheOwner};
            request = &mContext; mActive = true;
        }
        ~RequestScope() { if (mActive) request = mPrevious; }
    private:
        Context mContext;
        const Context* mPrevious = nullptr;
        bool mActive = false;
    };

    struct Stats
    {
        std::uint64_t publications = 0, active = 0, firstDemands = 0, prepareHits = 0, retirements = 0;
        std::uint64_t capacityFailures = 0, allocationFailures = 0, metadataTruncations = 0;
        std::uint64_t unknownOrigins = 0, unknownGenerations = 0, unknownUses = 0;
    };

    // Tokens are never reused. Cache items hold a token, not a payload reference.
    // Publication/retirement hooks run at the existing cache mutation, preventing
    // replacement or pointer reuse from being joined to the wrong publication.
    class Registry
    {
        struct Entry
        {
            const void* owner = nullptr;
            const void* resource = nullptr;
            const VFS::Manager* vfs = nullptr;
            std::uint64_t generation = 0, flags = 0, publicationSpan = 0;
            bool retired = false, demanded = false, preparationSeen = false;
            std::array<char, 64> identity{};
            std::array<char, 256> asset{};
        };
    public:
        // Small isolated registries exercise failure policies in tests without
        // mixing their local tokens into the production singleton's receipts.
        explicit Registry(std::size_t capacity = Capacity, bool receipts = true) noexcept
            : mCapacity(capacity), mReceipts(receipts) {}
        void initialize() noexcept
        {
            if (!enabled()) return;
            try { std::lock_guard lock(mMutex); storage(); }
            catch (...) { ++mUnexpectedFailures; }
        }
        Token publish(const void* owner, const void* resource, const Context& context) noexcept
        {
            if (!enabled() || !resource) return 0;
            HitchTrace::Scope overhead("diagnostic.resource_lifecycle");
            try
            {
                std::lock_guard lock(mMutex);
                if (!storage()) return 0;
                if (mStats.publications >= mCapacity) { ++mStats.capacityFailures; return 0; }
                const Token token = ++mStats.publications;
                Entry& entry = mEntries[token - 1];
                entry.owner = owner; entry.resource = resource; entry.vfs = context.vfs;
                entry.generation = context.generation;
                entry.flags = context.origin;
                if (!context.vfs || context.generation != context.vfs->getIndexGeneration())
                { entry.flags |= UnknownGeneration; ++mStats.unknownGenerations; }
                if (!context.origin) ++mStats.unknownOrigins;
                copy(entry.asset, context.asset);
                const auto kind = name(context.kind);
                std::memcpy(entry.identity.data(), kind.data(), kind.size());
                auto* cursor = entry.identity.data() + kind.size();
                constexpr std::string_view separator = ";cache=";
                std::memcpy(cursor, separator.data(), separator.size()); cursor += separator.size();
                std::to_chars(cursor, entry.identity.data() + entry.identity.size() - 1,
                    reinterpret_cast<std::uintptr_t>(owner), 16);
                ++mStats.active;
                std::optional<HitchTrace::Scope> publication;
                if (mReceipts)
                {
                    publication.emplace("resource.lifecycle_publish", entry.identity.data(), context.asset);
                    publication->retain(); publication->setLink(context.preparationSpan);
                    entry.publicationSpan = publication->id();
                    publication->setMetric(0, "lifecycle_id", token);
                    publication->setMetric(1, "vfs_generation", entry.generation);
                    publication->setMetric(2, "resource_ptr", reinterpret_cast<std::uintptr_t>(resource));
                    publication->setMetric(3, "source_flags", entry.flags);
                }
                if (context.origin == 1 && !(entry.flags & UnknownGeneration)) demand(entry, token, false);
                return token;
            }
            catch (...) { ++mUnexpectedFailures; return 0; }
        }
        void observe(Token token, const void* resource, const Context& context) noexcept
        {
            if (!enabled()) return;
            HitchTrace::Scope overhead("diagnostic.resource_lifecycle");
            try
            {
                std::lock_guard lock(mMutex);
                Entry* entry = get(token, resource);
                if (!entry || entry->retired || entry->owner != context.cacheOwner)
                { ++mStats.unknownUses; return; }
                if (entry->flags & UnknownGeneration) { ++mStats.unknownUses; return; }
                if (!context.vfs || context.vfs != entry->vfs || context.generation != entry->generation
                    || context.generation != context.vfs->getIndexGeneration())
                {
                    ++mStats.unknownUses;
                    if (!(entry->flags & UnknownGeneration))
                    {
                        entry->flags |= UnknownGeneration; ++mStats.unknownGenerations;
                        if (mReceipts)
                        {
                            HitchTrace::Scope invalid("resource.lifecycle_invalidation", entry->identity.data(), entry->asset.data());
                            invalid.retain(); invalid.setLink(entry->publicationSpan);
                            invalid.setMetric(0, "lifecycle_id", token);
                            invalid.setMetric(1, "observed_generation", context.generation);
                            invalid.setMetric(2, "reason_flags", (!context.vfs ? 1u : 0u)
                                | (context.vfs != entry->vfs ? 2u : 0u)
                                | (context.generation != entry->generation ? 4u : 0u)
                                | (context.vfs && context.generation != context.vfs->getIndexGeneration() ? 8u : 0u));
                            invalid.setMetric(3, "source_flags", entry->flags);
                        }
                    }
                    // A changed source identity cannot establish qualified
                    // preparation reuse or a first demand for this generation.
                    return;
                }
                if (context.origin == 1) demand(*entry, token, true);
                else if (context.origin == 2 && !entry->preparationSeen)
                {
                    entry->preparationSeen = true; entry->flags |= PreparationObserved; ++mStats.prepareHits;
                    if (!mReceipts) return;
                    HitchTrace::Scope hit("resource.lifecycle_prepare_hit", entry->identity.data(), entry->asset.data());
                    hit.retain(); hit.setLink(context.preparationSpan);
                    hit.setMetric(0, "lifecycle_id", token);
                    hit.setMetric(1, "resource_ptr", reinterpret_cast<std::uintptr_t>(resource));
                    hit.setMetric(2, "vfs_generation", entry->generation);
                    hit.setMetric(3, "publication_span", entry->publicationSpan);
                }
            }
            catch (...) { ++mUnexpectedFailures; }
        }
        void retire(Token token, const void* resource, Retirement reason,
            std::uint64_t remainingRefs = 0, std::uint64_t retainedEstimate = 0) noexcept
        {
            if (!enabled() || !token) return;
            HitchTrace::Scope overhead("diagnostic.resource_lifecycle");
            try
            {
                std::lock_guard lock(mMutex);
                Entry* entry = get(token, resource);
                if (!entry || entry->retired) return;
                entry->retired = true; --mStats.active; ++mStats.retirements;
                if (!mReceipts) return;
                HitchTrace::Scope retired("resource.lifecycle_retire", entry->identity.data(), entry->asset.data());
                retired.retain(); retired.setLink(entry->publicationSpan);
                retired.setMetric(0, "lifecycle_id", token);
                retired.setMetric(1, "reason", static_cast<std::uint64_t>(reason));
                retired.setMetric(2, "remaining_refs", remainingRefs);
                retired.setMetric(3, "retained_estimate", retainedEstimate);
            }
            catch (...) { ++mUnexpectedFailures; }
        }
        Stats stats() const noexcept
        {
            try
            {
                std::lock_guard lock(mMutex);
                auto result = mStats; result.allocationFailures += mUnexpectedFailures.load(); return result;
            }
            catch (...) { Stats result; result.allocationFailures = 1; return result; }
        }
    private:
        bool storage()
        {
            if (mEntries) return true;
            if (mAllocationFailed) return false;
            try { mEntries = std::make_unique<Entry[]>(mCapacity); return true; }
            catch (...) { mAllocationFailed = true; ++mStats.allocationFailures; return false; }
        }
        template <std::size_t N> void copy(std::array<char, N>& target, std::string_view source) noexcept
        {
            const auto count = (std::min)(source.size(), N - 1);
            if (count) std::memcpy(target.data(), source.data(), count);
            if (count != source.size()) ++mStats.metadataTruncations;
        }
        Entry* get(Token token, const void* resource) noexcept
        {
            if (!token || token > mStats.publications || !mEntries) return nullptr;
            Entry& entry = mEntries[token - 1];
            return entry.resource == resource ? &entry : nullptr;
        }
        void demand(Entry& entry, Token token, bool cacheHit) noexcept
        {
            if (entry.demanded) return;
            entry.demanded = true; ++mStats.firstDemands;
            if (!mReceipts) return;
            HitchTrace::Scope use("resource.lifecycle_first_demand", entry.identity.data(), entry.asset.data());
            use.retain(); use.setLink(entry.publicationSpan);
            use.setMetric(0, "lifecycle_id", token);
            use.setMetric(1, "resource_ptr", reinterpret_cast<std::uintptr_t>(entry.resource));
            use.setMetric(2, "cache_hit", cacheHit);
            use.setMetric(3, "source_flags", entry.flags);
        }
        const std::size_t mCapacity;
        const bool mReceipts;
        std::unique_ptr<Entry[]> mEntries;
        mutable std::mutex mMutex;
        Stats mStats;
        std::atomic<std::uint64_t> mUnexpectedFailures{0};
        bool mAllocationFailed = false;
    };

    inline Registry& registry() { static Registry value; return value; }
    inline void initialize() noexcept { if (enabled()) registry().initialize(); }
    inline Token publish(const void* owner, const void* resource) noexcept
    { return request && request->cacheOwner == owner ? registry().publish(owner, resource, *request) : 0; }
    inline void observe(Token token, const void* owner, const void* resource) noexcept
    { if (request && request->cacheOwner == owner) registry().observe(token, resource, *request); }
    inline void retire(Token token, const void* resource, Retirement reason,
        std::uint64_t references, std::uint64_t estimate = 0) noexcept
    { if (enabled()) registry().retire(token, resource, reason, references, estimate); }

    // Explicit engine hook: after workers join and before Capture::finish().
    // Always emits status when Deep is active, including zero-resource runs.
    inline void emitStatus() noexcept
    {
        if (!enabled()) return;
        const auto stats = registry().stats();
        HitchTrace::Scope status("resource.lifecycle_status"); status.retain();
        status.setMetric(0, "publications", stats.publications);
        status.setMetric(1, "capacity_failures", stats.capacityFailures);
        status.setMetric(2, "allocation_failures", stats.allocationFailures);
        status.setMetric(3, "metadata_truncations", stats.metadataTruncations);
        HitchTrace::Scope coverage("resource.lifecycle_coverage"); coverage.retain();
        coverage.setMetric(0, "active", stats.active);
        coverage.setMetric(1, "unknown_origins", stats.unknownOrigins);
        coverage.setMetric(2, "unknown_generations", stats.unknownGenerations);
        coverage.setMetric(3, "unknown_uses", stats.unknownUses);
    }
}

#endif
