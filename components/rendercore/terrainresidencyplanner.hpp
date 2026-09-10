#ifndef OPENMW_COMPONENTS_RENDERCORE_TERRAINRESIDENCYPLANNER_H
#define OPENMW_COMPONENTS_RENDERCORE_TERRAINRESIDENCYPLANNER_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace RenderCore
{
    struct TerrainResidencyCell
    {
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
        std::uint32_t lodLevel = 0;
        std::uint8_t stitchMask = 0;
        bool required = false;
        bool predicted = false;

        friend bool operator==(const TerrainResidencyCell&, const TerrainResidencyCell&) = default;
    };

    // Produces a compact resident neighborhood plus one row in the dominant
    // direction of travel. Direction persists while the player remains in the
    // same cell, avoiding request churn when frame-to-frame motion is small.
    class TerrainResidencyPlanner final
    {
    public:
        [[nodiscard]] std::vector<TerrainResidencyCell> update(
            std::string worldspace, std::int32_t gridX, std::int32_t gridY)
        {
            if (worldspace != mWorldspace)
            {
                mWorldspace = std::move(worldspace);
                mDirectionX = 0;
                mDirectionY = 0;
                mHasCenter = false;
            }
            if (mHasCenter && (gridX != mCenterX || gridY != mCenterY))
            {
                const std::int64_t deltaX = static_cast<std::int64_t>(gridX) - mCenterX;
                const std::int64_t deltaY = static_cast<std::int64_t>(gridY) - mCenterY;
                if (std::abs(deltaX) >= std::abs(deltaY))
                {
                    mDirectionX = deltaX < 0 ? -1 : 1;
                    mDirectionY = 0;
                }
                else
                {
                    mDirectionX = 0;
                    mDirectionY = deltaY < 0 ? -1 : 1;
                }
            }
            mCenterX = gridX;
            mCenterY = gridY;
            mHasCenter = true;

            std::vector<TerrainResidencyCell> result;
            result.reserve(mDirectionX == 0 && mDirectionY == 0 ? 25 : 30);
            for (std::int32_t y = -2; y <= 2; ++y)
            {
                for (std::int32_t x = -2; x <= 2; ++x)
                {
                    const bool inner = std::max(std::abs(x), std::abs(y)) <= 1;
                    std::uint8_t stitchMask = 0;
                    if (inner)
                    {
                        if (y == 1)
                            stitchMask |= 1u << 0;
                        if (x == 1)
                            stitchMask |= 1u << 1;
                        if (y == -1)
                            stitchMask |= 1u << 2;
                        if (x == -1)
                            stitchMask |= 1u << 3;
                    }
                    result.push_back({ gridX + x, gridY + y, inner ? 0u : 1u, stitchMask, x == 0 && y == 0, false });
                }
            }

            if (mDirectionX != 0)
            {
                for (std::int32_t y = -2; y <= 2; ++y)
                    result.push_back({ gridX + 3 * mDirectionX, gridY + y, 1u, 0u, false, true });
            }
            else if (mDirectionY != 0)
            {
                for (std::int32_t x = -2; x <= 2; ++x)
                    result.push_back({ gridX + x, gridY + 3 * mDirectionY, 1u, 0u, false, true });
            }
            return result;
        }

        void reset() noexcept
        {
            mWorldspace.clear();
            mHasCenter = false;
            mDirectionX = 0;
            mDirectionY = 0;
        }

    private:
        std::string mWorldspace;
        std::int32_t mCenterX = 0;
        std::int32_t mCenterY = 0;
        std::int32_t mDirectionX = 0;
        std::int32_t mDirectionY = 0;
        bool mHasCenter = false;
    };
}

#endif
