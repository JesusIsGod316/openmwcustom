#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GROUNDCOVER_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GROUNDCOVER_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct GroundcoverCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "Groundcover", "enabled" };
        SettingValue<float> mDensity{ mIndex, "Groundcover", "density", makeClampSanitizerFloat(0, 1) };
        SettingValue<float> mRenderingDistance{ mIndex, "Groundcover", "rendering distance", makeMaxSanitizerFloat(0) };
        SettingValue<int> mStompMode{ mIndex, "Groundcover", "stomp mode", makeEnumSanitizerInt({ 0, 1, 2 }) };
        SettingValue<int> mStompIntensity{ mIndex, "Groundcover", "stomp intensity",
            makeEnumSanitizerInt({ 0, 1, 2 }) };
        SettingValue<bool> mPointLighting{ mIndex, "Groundcover", "point lighting" };

        // OptimizedMW P8G groundcover GPU experiments.
        // 0 = stock Euler rotation + full wind, 1 = same-quality quaternion rotation
        //     plus early distance rejection, 2 = mode 1 plus reduced-harmonic wind probe.
        SettingValue<int> mOptimizedMWGpuPath{
            mIndex, "Groundcover", "optimizedmw gpu path",
            makeClampSanitizerInt(0, 2) };
        // Quality-risk ceiling probe. True preserves current shadow receiving.
        SettingValue<bool> mOptimizedMWShadowReceive{
            mIndex, "Groundcover", "optimizedmw shadow receive" };
    };
}

#endif
