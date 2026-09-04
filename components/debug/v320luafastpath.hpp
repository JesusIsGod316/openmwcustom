#ifndef OPENMW_COMPONENTS_DEBUG_V320LUAFASTPATH_H
#define OPENMW_COMPONENTS_DEBUG_V320LUAFASTPATH_H

#include <atomic>
#include <cstdint>

namespace Debug::V320LuaFastPath
{
    struct Counters
    {
        std::atomic<std::uint64_t> mEventChecks{ 0 };
        std::atomic<std::uint64_t> mNoHandlerFastPaths{ 0 };
        std::atomic<std::uint64_t> mActualDispatches{ 0 };
        std::atomic<std::uint64_t> mSoundConversionHits{ 0 };
        std::atomic<std::uint64_t> mSoundConversionMisses{ 0 };
        std::atomic<std::uint64_t> mSoundConversionEvictions{ 0 };
        std::atomic<std::uint64_t> mSoundQueryCalls{ 0 };
        std::atomic<std::uint64_t> mSoundQueryExecuted{ 0 };
        std::atomic<std::uint64_t> mSoundQueryCoalesced{ 0 };
        std::atomic<std::uint64_t> mSoundQueryDirtyInvalidations{ 0 };
    };

    inline Counters& counters()
    {
        static Counters value;
        return value;
    }

    inline void recordEventCheck(bool hasHandlers)
    {
        Counters& value = counters();
        value.mEventChecks.fetch_add(1, std::memory_order_relaxed);
        if (!hasHandlers)
            value.mNoHandlerFastPaths.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordDispatch()
    {
        counters().mActualDispatches.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundHit()
    {
        counters().mSoundConversionHits.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundMiss()
    {
        counters().mSoundConversionMisses.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundEviction()
    {
        counters().mSoundConversionEvictions.fetch_add(1, std::memory_order_relaxed);
    }
}

#endif
