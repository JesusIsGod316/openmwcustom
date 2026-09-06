#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICTEXTUREQUIRKS_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICTEXTUREQUIRKS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace RenderVsg
{
    // Matches Resource::ImageManager's Morrowind TGA compatibility rule. The
    // legacy renderer ignores the one-bit alpha channel on 16bpp TGA images even
    // when the header advertises it. Palette TGA variants store pixel depth at
    // byte 7; other supported TGA variants store it at byte 16.
    [[nodiscard]] inline bool openMwDiscard16BitTgaAlpha(const std::array<std::uint8_t, 18>& header) noexcept
    {
        const std::uint8_t type = header[2];
        const std::uint8_t depth = (type == 1u || type == 9u) ? header[7] : header[16];
        const std::uint8_t alphaBits = header[17] & 0x0fu;
        return depth == 16u && alphaBits == 1u;
    }

    [[nodiscard]] inline std::uint16_t readLittleU16(const std::uint8_t* bytes) noexcept
    {
        return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8u);
    }

    [[nodiscard]] inline std::uint32_t readLittleU32(const std::uint8_t* bytes) noexcept
    {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u)
            | (static_cast<std::uint32_t>(bytes[2]) << 16u) | (static_cast<std::uint32_t>(bytes[3]) << 24u);
    }

    // Mirrors OSG's dds_dxt1_detect_rgba test used by OpenMW. Only BC1/DXT1
    // blocks whose first endpoint is <= the second can encode transparent selector
    // 3. The presence of any selector 3 in such a block makes the image RGBA;
    // otherwise OpenMW realizes the texture as RGB DXT1. Caller supplies first-
    // mip BC1 blocks, eight bytes per block.
    [[nodiscard]] inline bool openMwBc1UsesOneBitAlpha(std::span<const std::uint8_t> blocks) noexcept
    {
        if (blocks.empty() || blocks.size() % 8u != 0u)
            return false;

        for (std::size_t offset = 0; offset < blocks.size(); offset += 8u)
        {
            const std::uint8_t* block = blocks.data() + offset;
            const std::uint16_t color0 = readLittleU16(block);
            const std::uint16_t color1 = readLittleU16(block + 2u);
            if (color0 > color1)
                continue;

            const std::uint32_t selectors = readLittleU32(block + 4u);
            for (std::uint32_t shift = 0u; shift < 32u; shift += 2u)
            {
                if (((selectors >> shift) & 0x03u) == 0x03u)
                    return true;
            }
        }
        return false;
    }
}

#endif
