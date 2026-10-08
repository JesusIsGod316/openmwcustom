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
            mIndex, "Groundcover", "shader optimization mode",
            makeClampSanitizerInt(0, 2) };
        // P8G3 startup-only, independently switchable. All derived topology is off by default.
        SettingValue<bool> mOptimizedMWHierarchy{ mIndex, "Groundcover", "spatial hierarchy" };
        SettingValue<bool> mOptimizedMWCullInputs{ mIndex, "Groundcover", "cull input reuse" };
        SettingValue<bool> mOptimizedMWLod2{ mIndex, "Groundcover", "geometry lod" };
        SettingValue<bool> mOptimizedMWDensityLod{ mIndex, "Groundcover", "density lod" };
        SettingValue<bool> mOptimizedMWFrontToBack{ mIndex, "Groundcover", "front to back ordering" };
        SettingValue<bool> mOptimizedMWParallelPrep{ mIndex, "Groundcover", "parallel preparation" };
        // -1 preserves the P8G2 mode-2 wind behavior; 0=full, 1=reduced independent of arithmetic path.
        SettingValue<int> mOptimizedMWFastWind{ mIndex, "Groundcover", "reduced wind", makeClampSanitizerInt(-1, 1) };
        SettingValue<int> mOptimizedMWMinBatch{ mIndex, "Groundcover", "minimum batch", makeClampSanitizerInt(32, 1024) };
        SettingValue<float> mOptimizedMWLodNear{ mIndex, "Groundcover", "lod near distance", makeClampSanitizerFloat(1000, 50000) };
        SettingValue<float> mOptimizedMWLodFar{ mIndex, "Groundcover", "lod far distance", makeClampSanitizerFloat(2000, 100000) };
        // Quality-risk ceiling probe. True preserves current shadow receiving.
        SettingValue<bool> mOptimizedMWShadowReceive{
            mIndex, "Groundcover", "receive shadows" };
    };
}

#endif
