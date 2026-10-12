#ifndef OPENMW_RESOURCE_ASSETPOLICY_H
#define OPENMW_RESOURCE_ASSETPOLICY_H

#include <cstdint>
#include <cstdlib>

namespace Resource::AssetPolicy
{
    enum Bits : unsigned
    {
        VisualLod = 1, Hlod = 2, TexturePreparation = 4,
        TextureStreaming = 8, TextureDemotion = 16
    };
    inline constexpr std::uint64_t MiB = 1024ull * 1024;
    inline constexpr std::uint64_t CpuTransientLimit = 256 * MiB;
    inline constexpr std::uint64_t NativePayloadLimit = 64 * MiB;
    inline constexpr std::uint64_t TextureAllocationLimit = 128 * MiB;
    inline constexpr std::uint64_t NewTextureLimit = 32 * MiB;
    inline constexpr std::uint64_t DiskLimit = 2048 * MiB;
    inline constexpr std::uint64_t DiskReserve = 20ull * 1024 * MiB;

    // Immutable for one engine process. Unknown/malformed masks fail to control.
    inline unsigned mask()
    {
        static const unsigned value = [] {
            const char* input = std::getenv("OPENMW_P9_ASSET_MASK");
            if (!input || !*input) return 0u;
            unsigned result = 0;
            for (const char* p = input; *p; ++p)
            {
                if (*p < '0' || *p > '9' || result > 31) return 0u;
                result = result * 10 + static_cast<unsigned>(*p - '0');
            }
            if (result > 31) return 0u;
            if (!(result & TextureStreaming)) result &= ~TextureDemotion;
            return result;
        }();
        return value;
    }
    inline bool enabled(unsigned bit) { return (mask() & bit) != 0; }
}
#endif
