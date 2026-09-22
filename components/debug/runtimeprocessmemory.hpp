#ifndef OPENMW_DEBUG_RUNTIMEPROCESSMEMORY_H
#define OPENMW_DEBUG_RUNTIMEPROCESSMEMORY_H
#include "runtimediagnostics.hpp"
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
namespace Debug::RuntimeDiagnostics
{
    inline void sampleProcessMemory() noexcept
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        const bool processOk = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != FALSE;
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        const bool memoryOk = GlobalMemoryStatusEx(&memory) != FALSE;
        PERFORMANCE_INFORMATION performance{};
        performance.cb = sizeof(performance);
        const bool commitOk = GetPerformanceInfo(&performance, sizeof(performance)) != FALSE;
        emit("os_memory", "windows", {}, {{"process_valid", processOk}, {"physical_valid", memoryOk},
            {"commit_valid", commitOk}, {"private_commit_bytes", process.PrivateUsage},
            {"working_set_bytes", process.WorkingSetSize}, {"peak_working_set_bytes", process.PeakWorkingSetSize},
            {"page_fault_count", process.PageFaultCount}, {"physical_total_bytes", memory.ullTotalPhys},
            {"physical_available_bytes", memory.ullAvailPhys},
            {"system_commit_bytes", static_cast<std::uint64_t>(performance.CommitTotal) * performance.PageSize},
            {"system_commit_limit_bytes", static_cast<std::uint64_t>(performance.CommitLimit) * performance.PageSize}});
#else
        emit("coverage", "os_memory", "Windows process counters unavailable on this platform", {{"available", 0}});
#endif
    }
}
#endif
