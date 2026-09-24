#ifndef OPENMW_MISC_ENVIRONMENTFLAG_H
#define OPENMW_MISC_ENVIRONMENTFLAG_H

#include <cstdlib>
#include <cstddef>

namespace Misc
{
    template <std::size_t N> struct EnvironmentFlagName
    {
        char value[N];
        constexpr EnvironmentFlagName(const char (&name)[N])
        { for (std::size_t i = 0; i < N; ++i) value[i] = name[i]; }
    };

    // Experiment selections belong to a process, not a draw. Keep the old
    // dynamic-environment control for exact same-executable measurements.
    // No pointer into the CRT environment escapes initialization.
    template <EnvironmentFlagName Name> bool environmentFlag()
    {
        static const bool cache = std::getenv("OPENMW_V4_STARTUP_FLAG_CACHE") != nullptr;
        if (cache)
        {
            static const bool selected = std::getenv(Name.value) != nullptr;
            return selected;
        }
        return std::getenv(Name.value) != nullptr;
    }
}
#endif
