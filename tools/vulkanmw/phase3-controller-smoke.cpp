#include <components/render/native/nifcontrollerprogram.hpp>

#include <cassert>
#include <cmath>

namespace
{
    bool near(float a, float b, float epsilon = 0.0001f)
    {
        return std::fabs(a - b) <= epsilon;
    }
}

int main()
{
    using namespace RenderNative;

    ControllerTiming cycle;
    cycle.start = 1.0f;
    cycle.stop = 3.0f;
    cycle.extrapolation = ControllerExtrapolation::Cycle;
    assert(near(cycle.map(3.5f), 1.5f));
    assert(near(cycle.map(0.5f), 2.5f));

    ControllerTiming reverse = cycle;
    reverse.extrapolation = ControllerExtrapolation::Reverse;
    assert(near(reverse.map(3.5f), 2.5f));
    assert(near(reverse.map(5.5f), 1.5f));

    ControllerTiming constant = cycle;
    constant.extrapolation = ControllerExtrapolation::Constant;
    assert(near(constant.map(0.0f), 1.0f));
    assert(near(constant.map(9.0f), 3.0f));

    FloatControllerTrack linear;
    linear.keys = {
        ControllerKey<float>{ .time = 0.0f, .value = 2.0f },
        ControllerKey<float>{ .time = 2.0f, .value = 6.0f },
    };
    assert(near(*linear.sample(1.0f), 4.0f));

    FloatControllerTrack constantTrack = linear;
    constantTrack.interpolation = ControllerInterpolation::Constant;
    assert(near(*constantTrack.sample(0.9f), 2.0f));
    assert(near(*constantTrack.sample(1.1f), 6.0f));

    FloatControllerTrack hermite;
    hermite.interpolation = ControllerInterpolation::Hermite;
    hermite.keys = {
        ControllerKey<float>{ .time = 0.0f, .value = 0.0f, .inTangent = 0.0f, .outTangent = 0.0f },
        ControllerKey<float>{ .time = 1.0f, .value = 1.0f, .inTangent = 0.0f, .outTangent = 0.0f },
    };
    assert(near(*hermite.sample(0.5f), 0.5f));

    Vec3ControllerTrack vecTrack;
    vecTrack.keys = {
        ControllerKey<glm::vec3>{ .time = 0.0f, .value = glm::vec3(0.0f) },
        ControllerKey<glm::vec3>{ .time = 1.0f, .value = glm::vec3(2.0f, 4.0f, 6.0f) },
    };
    const glm::vec3 vec = *vecTrack.sample(0.5f);
    assert(near(vec.x, 1.0f) && near(vec.y, 2.0f) && near(vec.z, 3.0f));

    BoolControllerTrack visibility;
    visibility.legacyStepPrevious = true;
    visibility.keys = {
        BoolControllerKey{ .time = 0.0f, .value = true },
        BoolControllerKey{ .time = 0.75f, .value = false },
    };
    assert(*visibility.sample(0.5f));
    assert(!*visibility.sample(0.9f));

    NifControllerProgram program;
    TransformControllerProgram transform;
    transform.node = RenderCore::ModelNodeIndex{ 0 };
    transform.timing = constant;
    transform.autoPlay = true;
    transform.track.translations = vecTrack;
    program.transforms.push_back(transform);
    assert(program.valid());
    assert(!program.empty());

    return 0;
}
