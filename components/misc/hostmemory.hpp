#ifndef OPENMW_COMPONENTS_MISC_HOSTMEMORY_H
#define OPENMW_COMPONENTS_MISC_HOSTMEMORY_H

#include <cstdint>

namespace Misc
{
    // Bytes with independent validity flags, not a working-set/VRAM sum.
    struct HostMemoryStatus
    {
        std::uint64_t physicalTotal = 0;
        std::uint64_t physicalAvailable = 0;
        std::uint64_t privateCommit = 0;
        std::uint64_t commitAvailable = 0;
        bool physicalValid = false;
        bool processValid = false;
        bool commitValid = false;
    };

    // No platform headers or diagnostic recorder dependency at this boundary.
    HostMemoryStatus queryHostMemoryStatus() noexcept;
}
#endif
