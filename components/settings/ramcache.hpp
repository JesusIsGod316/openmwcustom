#ifndef OPENMW_COMPONENTS_SETTINGS_RAMCACHE_H
#define OPENMW_COMPONENTS_SETTINGS_RAMCACHE_H

#include <algorithm>
#include <string>
#include <string_view>

#include "values.hpp"

namespace Settings::RamCache
{
    enum class Mode
    {
        Normal,
        Aggressive,
        Extreme,
    };

    inline Mode mode()
    {
        const std::string value = cells().mRamCacheMode;
        if (value == "extreme")
            return Mode::Extreme;
        if (value == "aggressive")
            return Mode::Aggressive;
        return Mode::Normal;
    }

    inline std::string_view name()
    {
        switch (mode())
        {
            case Mode::Aggressive:
                return "aggressive";
            case Mode::Extreme:
                return "extreme";
            case Mode::Normal:
            default:
                return "normal";
        }
    }

    // Presets are implemented as floors, not hard overrides. Users who already
    // specify even larger cache values keep those values. "normal" changes
    // nothing and therefore preserves upstream OpenMW behavior exactly.
    inline int preloadCellCacheMin()
    {
        const int configured = cells().mPreloadCellCacheMin;
        switch (mode())
        {
            case Mode::Aggressive:
                return std::max(configured, 32);
            case Mode::Extreme:
                return std::max(configured, 64);
            default:
                return configured;
        }
    }

    inline int preloadCellCacheMax()
    {
        const int configured = cells().mPreloadCellCacheMax;
        switch (mode())
        {
            case Mode::Aggressive:
                return std::max(configured, 64);
            case Mode::Extreme:
                return std::max(configured, 128);
            default:
                return configured;
        }
    }

    inline float preloadCellExpiryDelay()
    {
        const float configured = cells().mPreloadCellExpiryDelay;
        switch (mode())
        {
            case Mode::Aggressive:
                return std::max(configured, 120.f);
            case Mode::Extreme:
                return std::max(configured, 600.f);
            default:
                return configured;
        }
    }

    inline float cacheExpiryDelay()
    {
        const float configured = cells().mCacheExpiryDelay;
        switch (mode())
        {
            case Mode::Aggressive:
                return std::max(configured, 120.f);
            case Mode::Extreme:
                return std::max(configured, 600.f);
            default:
                return configured;
        }
    }

    inline float predictionTime()
    {
        const float configured = cells().mPredictionTime;
        switch (mode())
        {
            case Mode::Aggressive:
                return std::max(configured, 2.f);
            case Mode::Extreme:
                return std::max(configured, 2.f);
            default:
                return configured;
        }
    }

    inline bool preloadInstances()
    {
        return mode() == Mode::Normal ? static_cast<bool>(cells().mPreloadInstances) : true;
    }
}

#endif
