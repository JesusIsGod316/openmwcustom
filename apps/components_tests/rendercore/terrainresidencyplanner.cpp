#include <components/rendercore/terrainresidencyplanner.hpp>

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
    TEST(TerrainResidencyPlanner, StartsWithRequiredThreeByThreeNeighborhood)
    {
        RenderCore::TerrainResidencyPlanner planner;
        const auto cells = planner.update("world", 4, -2);
        EXPECT_EQ(cells.size(), 9u);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.required; }), 1);
        EXPECT_EQ(std::ranges::count_if(cells, [](const auto& cell) { return cell.predicted; }), 0);
    }

    TEST(TerrainResidencyPlanner, PersistsDominantTravelPredictionWithoutFrameChurn)
    {
        RenderCore::TerrainResidencyPlanner planner;
        static_cast<void>(planner.update("world", 0, 0));
        const auto moved = planner.update("world", 1, 0);
        const auto stationary = planner.update("world", 1, 0);
        ASSERT_EQ(moved.size(), 12u);
        EXPECT_EQ(stationary.size(), moved.size());
        EXPECT_EQ(std::ranges::count_if(moved, [](const auto& cell) { return cell.predicted && cell.gridX == 3; }), 3);
        EXPECT_TRUE(std::ranges::equal(moved, stationary));
    }

    TEST(TerrainResidencyPlanner, ClearsPredictionAcrossWorldspaceTransition)
    {
        RenderCore::TerrainResidencyPlanner planner;
        static_cast<void>(planner.update("world-a", 0, 0));
        ASSERT_EQ(planner.update("world-a", 0, -1).size(), 12u);
        const auto changed = planner.update("world-b", 40, 40);
        EXPECT_EQ(changed.size(), 9u);
        EXPECT_EQ(std::ranges::count_if(changed, [](const auto& cell) { return cell.predicted; }), 0);
    }
}
