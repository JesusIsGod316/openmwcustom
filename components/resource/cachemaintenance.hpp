#ifndef OPENMW_COMPONENTS_RESOURCE_CACHEMAINTENANCE_H
#define OPENMW_COMPONENTS_RESOURCE_CACHEMAINTENANCE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Resource
{
    // One budget for ALL participating managers in a pass, not a fresh 4096
    // entries for each manager. Time is checked between safe units; destructors
    // and graphics-driver calls are not preemptible. No extra work threads.
    class CacheMaintenanceBudget
    {
    public:
        using Clock = std::chrono::steady_clock;
        explicit CacheMaintenanceBudget(std::size_t scans = 4096, std::size_t releases = 64,
            std::chrono::microseconds duration = std::chrono::microseconds(2000))
            : mScans(scans), mReleases(releases), mDeadline(Clock::now() + duration) {}
        bool available() const { return mScans && Clock::now() < mDeadline; }
        bool scan() { if (!available()) return false; --mScans; ++scanned; return true; }
        bool release(std::uint64_t bytes = 0)
        {
            constexpr std::uint64_t byteLimit = 64ull * 1024 * 1024;
            if (!mReleases || Clock::now() >= mDeadline || (releasedBytes &&
                (releasedBytes >= byteLimit || bytes > byteLimit - releasedBytes))) return false;
            --mReleases; ++released;
            releasedBytes = bytes > UINT64_MAX - releasedBytes ? UINT64_MAX : releasedBytes + bytes;
            return true;
        }
        std::uint64_t releasedBytes = 0;
        std::size_t scanned = 0, released = 0;
    private:
        std::size_t mScans, mReleases;
        Clock::time_point mDeadline;
    };
    class CacheMaintenanceScope
    {
    public:
        explicit CacheMaintenanceScope(CacheMaintenanceBudget& budget) : mPrevious(sBudget) { sBudget = &budget; }
        ~CacheMaintenanceScope() { sBudget = mPrevious; }
        CacheMaintenanceScope(const CacheMaintenanceScope&) = delete;
        CacheMaintenanceScope& operator=(const CacheMaintenanceScope&) = delete;
        static CacheMaintenanceBudget* current() noexcept { return sBudget; }
    private:
        inline static thread_local CacheMaintenanceBudget* sBudget = nullptr;
        CacheMaintenanceBudget* mPrevious;
    };
}
#endif
