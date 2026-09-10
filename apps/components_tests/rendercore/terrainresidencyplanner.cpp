#include <components/rendercore/terrainresidencyplanner.hpp>

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
    TEST(TerrainResidencyPlanner, StartsWithRequiredThreeByThreeNeighborhood)
    {
        RenderCore::TerrainResidencyPlanner planner;
        const auto cells = planner.update("world", 4, -2);
        EXPECT_EQ(cells.size(), 25u);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.required; }), 1);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.predicted; }), 0);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.lodLevel == 0; }), 9);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.lodLevel == 1; }), 16);
        const auto northEast
            = std::ranges::find_if(cells, [](const auto& cell) { return cell.gridX == 5 && cell.gridY == -1; });
        ASSERT_NE(northEast, cells.end());
        EXPECT_EQ(northEast->stitchMask, (1u << 0) | (1u << 1));
        const auto center
            = std::ranges::find_if(cells, [](const auto& cell) { return cell.gridX == 4 && cell.gridY == -2; });
        ASSERT_NE(center, cells.end());
        EXPECT_TRUE(center->required);
        EXPECT_EQ(center->stitchMask, 0u);
    }

    TEST(TerrainResidencyPlanner, PersistsDominantTravelPredictionWithoutFrameChurn)
    {
        RenderCore::TerrainResidencyPlanner planner;
        static_cast<void>(planner.update("world", 0, 0));
        const auto moved = planner.update("world", 1, 0);
        const auto stationary = planner.update("world", 1, 0);
        ASSERT_EQ(moved.size(), 30u);
        EXPECT_EQ(stationary.size(), moved.size());
        EXPECT_EQ(std::ranges::count_if(moved, [](const auto& cell) { return cell.predicted && cell.gridX == 4; }), 5);
        EXPECT_EQ(std::ranges::count_if(moved,
                      [](const auto& cell) { return cell.predicted && cell.lodLevel == 1 && cell.stitchMask == 0; }),
            5);
        EXPECT_TRUE(std::ranges::equal(moved, stationary));
    }

    TEST(TerrainResidencyPlanner, ClearsPredictionAcrossWorldspaceTransition)
    {
        RenderCore::TerrainResidencyPlanner planner;
        static_cast<void>(planner.update("world-a", 0, 0));
        ASSERT_EQ(planner.update("world-a", 0, -1).size(), 30u);
        const auto changed = planner.update("world-b", 40, 40);
        EXPECT_EQ(changed.size(), 25u);
        EXPECT_EQ(std::ranges::count_if(changed, [](const auto& cell) { return cell.predicted; }), 0);
    }
}
