#include "hitchtrace.hpp"

#ifdef _WIN32
#include <components/misc/windows.hpp>
#else
#include <time.h>
#endif

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
