#ifndef OPENMW_SCENEUTIL_SHADOWSETTINGSUPDATE_H
#define OPENMW_SCENEUTIL_SHADOWSETTINGSUPDATE_H

#include <osg/Uniform>
#include <osg/Vec2s>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace SceneUtil
{
    struct ShadowRuntimeParameters
    {
        float distance;
        float fadeStart;
        short resolution;
    };

    inline std::optional<ShadowRuntimeParameters> shadowRuntimeParameters(float distance, float fade, int resolution)
    {
        if (!std::isfinite(distance) || !std::isfinite(fade) || resolution < 1
            || resolution > std::numeric_limits<short>::max())
            return std::nullopt;
        if (distance <= 0.f)
            return ShadowRuntimeParameters{std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max() * 0.5f, static_cast<short>(resolution)};
        // A fade fraction of 1 means an effectively hard cutoff, not 0/0 in GLSL.
        const float fadeDistance = std::min(distance * std::clamp(fade, 0.f, 1.f), std::nextafter(distance, 0.f));
        return ShadowRuntimeParameters{distance, fadeDistance, static_cast<short>(resolution)};
    }

    inline osg::Vec2s shadowTextureExtent(osg::Vec2s extent, bool debug,
        unsigned index, unsigned count, unsigned farDivisor)
    {
        if (debug) return osg::Vec2s(512, 512);
        if (farDivisor > 1 && count > 1 && index + 1 == count)
        {
            extent.set(static_cast<short>(std::max(1, static_cast<int>(extent.x()) / static_cast<int>(farDivisor))),
                static_cast<short>(std::max(1, static_cast<int>(extent.y()) / static_cast<int>(farDivisor))));
        }
        return extent;
    }

    template<class FrameUniformLists>
    void replaceShadowUniform(FrameUniformLists& lists, const char* name, float value)
    {
        // Existing render states may still retain the previous uniform. Publish a
        // replacement to future culls instead of mutating that captured value.
        osg::ref_ptr<osg::Uniform> replacement = new osg::Uniform(name, value);
        for (auto& list : lists)
            for (auto& uniform : list)
                if (uniform->getName() == name) uniform = replacement;
    }
}
#endif
