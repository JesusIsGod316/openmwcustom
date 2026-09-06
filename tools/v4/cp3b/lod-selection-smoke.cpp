#include <components/rendercore/lodselection.hpp>

#include <cassert>
#include <limits>

int main()
{
    using namespace RenderCore;

    ModelLodSemantic lod;
    lod.ranges.push_back(ModelLodRange{ ModelNodeIndex{ 1u }, 0.0f, 100.0f });
    lod.ranges.push_back(ModelLodRange{ ModelNodeIndex{ 2u }, 50.0f, 150.0f });
    lod.ranges.push_back(ModelLodRange{ ModelNodeIndex{ 3u }, 75.0f, 125.0f });

    const auto nearChild = selectModelLodChild(lod, 25.0f);
    assert(nearChild && nearChild->value() == 1u);

    const auto overlapTwo = selectModelLodChild(lod, 60.0f);
    assert(overlapTwo && overlapTwo->value() == 2u);

    const auto overlapThree = selectModelLodChild(lod, 80.0f);
    assert(overlapThree && overlapThree->value() == 3u);

    const auto farChild = selectModelLodChild(lod, 140.0f);
    assert(farChild && farChild->value() == 2u);

    assert(!selectModelLodChild(lod, 151.0f));
    assert(!selectModelLodChild(lod, std::numeric_limits<float>::quiet_NaN()));

    return 0;
}
