#ifndef OPENMW_SCENEUTIL_SHADOWBATCHCOUNTERS_H
#define OPENMW_SCENEUTIL_SHADOWBATCHCOUNTERS_H
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>

namespace SceneUtil::ShadowBatchCounters
{
    inline bool enabled()
    {
        static const bool value = [] { const char* p = std::getenv("OPENMW_P8G4_STATS"); return p && *p == '1'; }();
        return value;
    }
    inline std::atomic_uint64_t built{ 0 }, wrapped{ 0 }, builtIndices{ 0 };
    inline std::array<std::atomic_uint64_t, 8> proxyVisits{}, normalFallback{}, proxyIndices{};
    inline void add(std::atomic_uint64_t& counter, std::uint64_t value = 1)
    {
        if (enabled()) counter.fetch_add(value, std::memory_order_relaxed);
    }
}
#endif
