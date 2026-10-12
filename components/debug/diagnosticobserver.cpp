#include "diagnosticobserver.hpp"

#ifdef _WIN32
#include <Windows.h>
#else
#include <functional>
#include <thread>
#endif

namespace Debug::DiagnosticObserver
{
    std::uint64_t currentNativeThreadId() noexcept
    {
#ifdef _WIN32
        return static_cast<std::uint64_t>(GetCurrentThreadId());
#else
        return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }
}
