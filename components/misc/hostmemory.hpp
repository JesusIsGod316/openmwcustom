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
        // Optional independent signals; appended to retain old aggregate layout.
        std::uint64_t systemCommitAvailable = 0;
        bool systemCommitValid = false;
        bool lowMemory = false;
        bool lowMemoryValid = false;
    };

    // No platform headers or diagnostic recorder dependency at this boundary.
    HostMemoryStatus queryHostMemoryStatus() noexcept;
    // GL-P1A sampler only. Legacy callers retain their existing query behavior.
    HostMemoryStatus queryOpenGlHostMemoryStatus() noexcept;
}
#endif
