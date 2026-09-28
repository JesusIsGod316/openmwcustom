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

        // OptimizedMW P8G2 shader-only groundcover experiments.
        // 0 = exact stock shader path, 1 = same-quality arithmetic/branch fast path,
        // 2 = mode 1 + reduced-harmonic wind ceiling probe.
        SettingValue<int> mOptimizedMWGpuPath{
            mIndex, "Groundcover", "optimizedmw gpu path",
            makeClampSanitizerInt(0, 2) };
        // P8G3 startup-only, independently switchable. All derived topology is off by default.
        SettingValue<bool> mOptimizedMWHierarchy{ mIndex, "Groundcover", "optimizedmw hierarchy" };
        SettingValue<bool> mOptimizedMWCullInputs{ mIndex, "Groundcover", "optimizedmw cull input reuse" };
        SettingValue<bool> mOptimizedMWLod2{ mIndex, "Groundcover", "optimizedmw lod2" };
        SettingValue<bool> mOptimizedMWDensityLod{ mIndex, "Groundcover", "optimizedmw density lod" };
        SettingValue<bool> mOptimizedMWFrontToBack{ mIndex, "Groundcover", "optimizedmw front to back" };
        SettingValue<bool> mOptimizedMWParallelPrep{ mIndex, "Groundcover", "optimizedmw parallel preparation" };
        // -1 preserves the P8G2 mode-2 wind behavior; 0=full, 1=reduced independent of arithmetic path.
        SettingValue<int> mOptimizedMWFastWind{ mIndex, "Groundcover", "optimizedmw fast wind", makeClampSanitizerInt(-1, 1) };
        SettingValue<int> mOptimizedMWMinBatch{ mIndex, "Groundcover", "optimizedmw minimum batch", makeClampSanitizerInt(32, 1024) };
        SettingValue<float> mOptimizedMWLodNear{ mIndex, "Groundcover", "optimizedmw lod near", makeClampSanitizerFloat(1000, 50000) };
        SettingValue<float> mOptimizedMWLodFar{ mIndex, "Groundcover", "optimizedmw lod far", makeClampSanitizerFloat(2000, 100000) };
        // Quality-risk ceiling probe. True preserves current shadow receiving.
        SettingValue<bool> mOptimizedMWShadowReceive{
            mIndex, "Groundcover", "optimizedmw shadow receive" };
    };
}

#endif
