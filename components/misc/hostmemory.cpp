#include "hostmemory.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace Misc
{
    HostMemoryStatus queryHostMemoryStatus() noexcept
    {
        HostMemoryStatus result;
#ifdef _WIN32
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
        {
            result.physicalTotal = memory.ullTotalPhys;
            result.physicalAvailable = memory.ullAvailPhys;
            result.physicalValid = memory.ullTotalPhys != 0 && memory.ullAvailPhys <= memory.ullTotalPhys;
            // This is the current process's available commit, which may be
            // smaller than system-wide headroom. It is not free physical RAM.
            result.commitAvailable = memory.ullAvailPageFile;
            result.commitValid = true;
        }
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
        {
            result.privateCommit = process.PrivateUsage;
            result.processValid = true;
        }
#endif
        // Unavailable counters do not mean zero available bytes or an OOM.
        return result;
    }
    HostMemoryStatus queryOpenGlHostMemoryStatus() noexcept
    {
        auto result = queryHostMemoryStatus();
#ifdef _WIN32
        PERFORMANCE_INFORMATION performance{};
        performance.cb = sizeof(performance);
        if (GetPerformanceInfo(&performance, sizeof(performance)) && performance.PageSize != 0
            && performance.CommitLimit >= performance.CommitTotal)
        {
            const auto pages = performance.CommitLimit - performance.CommitTotal;
            if (pages <= UINT64_MAX / performance.PageSize)
            {
                result.systemCommitAvailable = static_cast<std::uint64_t>(pages) * performance.PageSize;
                result.systemCommitValid = true;
            }
        }
        struct Notification
        {
            HANDLE handle = CreateMemoryResourceNotification(LowMemoryResourceNotification);
            ~Notification() { if (handle) CloseHandle(handle); }
        };
        // Constructed once by the startup/sampler owner, never by a frame read.
        static const Notification notification;
        BOOL low = FALSE;
        if (notification.handle && QueryMemoryResourceNotification(notification.handle, &low))
        {
            result.lowMemory = low != FALSE;
            result.lowMemoryValid = true;
        }
#endif
        return result;
    }

}
