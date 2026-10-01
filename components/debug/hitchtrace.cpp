#include "hitchtrace.hpp"

#ifdef _WIN32
#include <components/misc/windows.hpp>
#else
#include <time.h>
#endif

std::uint64_t Debug::HitchTrace::threadId() noexcept
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

void Debug::HitchTrace::writeClockMap(const std::string& path, bool final) noexcept
{
    try
    {
        std::ofstream out(path + ".clock-map.txt", final ? std::ios::app : std::ios::trunc);
#ifdef _WIN32
        LARGE_INTEGER frequency{}, before{}, after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        QueryPerformanceCounter(&after);
        const char* prefix = final ? "end_" : "start_";
        if (!final)
            out << "schema=1\nprocess_id=" << GetCurrentProcessId()
                << "\nthread_id_kind=windows_native_tid\nqpc_frequency=" << frequency.QuadPart
                << "\nthread_cpu_resolution=coarse_GetThreadTimes_not_submillisecond\n";
        out << prefix << "qpc_before=" << before.QuadPart << '\n'
            << prefix << "steady_ns=" << steady << '\n'
            << prefix << "utc_ns=" << utc << '\n'
            << prefix << "qpc_after=" << after.QuadPart << '\n';
#else
        if (!final) out << "schema=1\nthread_id_kind=std_thread_hash_no_ETW_mapping\n";
#endif
        out << (final ? "normal_finish=1\n" : "started=1\n");
    }
    catch (...) { /* Missing clock map explicitly prevents ETW alignment. */ }
}

std::int64_t Debug::HitchTrace::threadCpuNs() noexcept
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<std::int64_t>((ticks(kernel) + ticks(user)) * 100);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return -1;
    return t.tv_sec * 1000000000LL + t.tv_nsec;
#else
    return -1;
#endif
}
