#include "../../openmw/mwrender/hybridanimationtimemap.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace
{
    using MWRender::HybridTimeAnchor;

    TEST(HybridAnimationTimeMap, AlignsAttackPhasesAcrossDifferentClipTimelines)
    {
        const std::vector<HybridTimeAnchor> anchors{ { 4.f, 15.5f }, { 4.4f, 16.f }, { 4.8f, 17.f } };
        EXPECT_FLOAT_EQ(MWRender::mapHybridAnimationTime(anchors, 4.f), 15.5f);
        EXPECT_NEAR(MWRender::mapHybridAnimationTime(anchors, 4.2f), 15.75f, 0.0001f);
        EXPECT_FLOAT_EQ(MWRender::mapHybridAnimationTime(anchors, 4.4f), 16.f);
        EXPECT_NEAR(MWRender::mapHybridAnimationTime(anchors, 4.6f), 16.5f, 0.0001f);
        EXPECT_FLOAT_EQ(MWRender::mapHybridAnimationTime(anchors, 5.f), 17.f);
    }

    TEST(HybridAnimationTimeMap, RejectsNonmonotoneOrInvalidPhaseMatches)
    {
        std::vector<HybridTimeAnchor> anchors;
        EXPECT_TRUE(MWRender::appendHybridTimeAnchor(anchors, 1.f, 10.f));
        EXPECT_FALSE(MWRender::appendHybridTimeAnchor(anchors, 1.f, 11.f));
        EXPECT_FALSE(MWRender::appendHybridTimeAnchor(anchors, 2.f, 9.f));
        EXPECT_FALSE(MWRender::appendHybridTimeAnchor(
            anchors, std::numeric_limits<float>::quiet_NaN(), 11.f));
        EXPECT_TRUE(MWRender::appendHybridTimeAnchor(anchors, 2.f, 12.f));
        EXPECT_EQ(anchors.size(), 2u);
        EXPECT_FLOAT_EQ(MWRender::mapHybridAnimationTime(anchors, 1.5f), 11.f);
    }
}
