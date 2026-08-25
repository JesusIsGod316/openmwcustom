#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_TELEMETRY_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_TELEMETRY_H

#include <atomic>
#include <cstdint>

namespace OcclusionCulling::Telemetry
{
    enum class CacheCounterId
    {
        MemHits = 0,
        DbHits,
        Misses,
        Writes,
    };

    inline std::atomic<std::uint64_t>& lifetimeCounter(CacheCounterId id)
    {
        static std::atomic<std::uint64_t> memHits{ 0 };
        static std::atomic<std::uint64_t> dbHits{ 0 };
        static std::atomic<std::uint64_t> misses{ 0 };
        static std::atomic<std::uint64_t> writes{ 0 };

        switch (id)
        {
            case CacheCounterId::MemHits:
                return memHits;
            case CacheCounterId::DbHits:
                return dbHits;
            case CacheCounterId::Misses:
                return misses;
            case CacheCounterId::Writes:
                return writes;
        }
        return memHits;
    }

    template <CacheCounterId Id>
    class CacheCounter
    {
    public:
        CacheCounter& operator++() noexcept
        {
            mInterval.fetch_add(1, std::memory_order_relaxed);
            lifetimeCounter(Id).fetch_add(1, std::memory_order_relaxed);
            return *this;
        }

        int exchange(int value) noexcept
        {
            return mInterval.exchange(value, std::memory_order_relaxed);
        }

    private:
        std::atomic<int> mInterval{ 0 };
    };

    struct CacheStats
    {
        std::uint64_t memHits = 0;
        std::uint64_t dbHits = 0;
        std::uint64_t misses = 0;
        std::uint64_t writes = 0;
    };

    inline CacheStats getLifetimeCacheStats() noexcept
    {
        return {
            lifetimeCounter(CacheCounterId::MemHits).load(std::memory_order_relaxed),
            lifetimeCounter(CacheCounterId::DbHits).load(std::memory_order_relaxed),
            lifetimeCounter(CacheCounterId::Misses).load(std::memory_order_relaxed),
            lifetimeCounter(CacheCounterId::Writes).load(std::memory_order_relaxed),
        };
    }
}

#endif
