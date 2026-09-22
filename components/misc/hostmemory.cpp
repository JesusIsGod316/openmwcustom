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
}
