#ifndef OPENMW_COMPONENTS_SETTINGS_RAMCACHE_H
#define OPENMW_COMPONENTS_SETTINGS_RAMCACHE_H

#include <algorithm>
#include <cstddef>
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
        Overdrive,
    };

    enum class OverdrivePreload
    {
        Balanced,
        Aggressive,
        Maximum,
    };

    inline Mode mode()
    {
        const std::string value = cells().mRamCacheMode;
        if (value == "overdrive")
            return Mode::Overdrive;
        if (value == "extreme")
            return Mode::Extreme;
        if (value == "aggressive")
            return Mode::Aggressive;
        return Mode::Normal;
    }

    inline OverdrivePreload overdrivePreload()
    {
        const std::string value = cells().mRamCacheOverdrivePreload;
        if (value == "maximum")
            return OverdrivePreload::Maximum;
        if (value == "aggressive")
            return OverdrivePreload::Aggressive;
        return OverdrivePreload::Balanced;
    }

    inline std::string_view name()
    {
        switch (mode())
        {
            case Mode::Aggressive:
                return "aggressive";
            case Mode::Extreme:
                return "extreme";
            case Mode::Overdrive:
                return "overdrive";
            case Mode::Normal:
            default:
                return "normal";
        }
    }

    inline std::string_view overdrivePreloadName()
    {
        switch (overdrivePreload())
        {
            case OverdrivePreload::Aggressive:
                return "aggressive";
            case OverdrivePreload::Maximum:
                return "maximum";
            case OverdrivePreload::Balanced:
            default:
                return "balanced";
        }
    }

    // Presets are implemented as floors, not hard caps. "normal" changes
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
            case Mode::Overdrive:
                switch (overdrivePreload())
                {
                    case OverdrivePreload::Aggressive:
                        return std::max(configured, 128);
                    case OverdrivePreload::Maximum:
                        return std::max(configured, 160);
                    case OverdrivePreload::Balanced:
                    default:
                        return std::max(configured, 96);
                }
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
            case Mode::Overdrive:
                switch (overdrivePreload())
                {
                    case OverdrivePreload::Aggressive:
                        return std::max(configured, 224);
                    case OverdrivePreload::Maximum:
                        return std::max(configured, 256);
                    case OverdrivePreload::Balanced:
                    default:
                        return std::max(configured, 192);
                }
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
            case Mode::Overdrive:
                return std::max(configured, 1800.f);
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
            case Mode::Overdrive:
                return std::max(configured, 1800.f);
            default:
                return configured;
        }
    }

    // These managers bypass ResourceSystem's common expiry policy upstream, so
    // expose explicit effective delays for the optimization lab.
    inline float terrainExpiryDelay() { return cacheExpiryDelay(); }
    inline float objectPagingExpiryDelay() { return cacheExpiryDelay(); }
    inline float groundcoverExpiryDelay() { return cacheExpiryDelay(); }

    inline bool retainNifFiles()
    {
        // Raw parsed NIFs normally expire immediately after converted scene/
        // collision objects are cached. Overdrive intentionally trades RAM for
        // avoiding reparsing when paging systems request the same source again.
        return mode() == Mode::Overdrive;
    }

    // MultiObjectCache normally discards every unused BulletShapeInstance on
    // the next resource-cache sweep. Keeping a bounded pool is useful on
    // 32-GB systems because exterior cells repeatedly request the same shapes.
    inline std::size_t shapeInstancePoolSize()
    {
        switch (mode())
        {
            case Mode::Extreme:
                return 16384;
            case Mode::Overdrive:
                return 65536;
            default:
                return 0;
        }
    }

    inline float predictionTime()
    {
        const float configured = cells().mPredictionTime;
        switch (mode())
        {
            case Mode::Aggressive:
            case Mode::Extreme:
                return std::max(configured, 2.f);
            case Mode::Overdrive:
                switch (overdrivePreload())
                {
                    case OverdrivePreload::Aggressive:
                        return std::max(configured, 3.f);
                    case OverdrivePreload::Maximum:
                        return std::max(configured, 4.f);
                    case OverdrivePreload::Balanced:
                    default:
                        return std::max(configured, 2.f);
                }
            default:
                return configured;
        }
    }

    inline bool preloadInstances()
    {
        if (mode() == Mode::Normal)
            return static_cast<bool>(cells().mPreloadInstances);

        // Preserve the existing aggressive/extreme behavior. Overdrive's
        // balanced policy still respects an explicit user choice, while its
        // higher preload policies force pre-instancing on.
        if (mode() == Mode::Overdrive && overdrivePreload() == OverdrivePreload::Balanced)
            return static_cast<bool>(cells().mPreloadInstances);
        return true;
    }

    // Optional V3 walking-stutter experiment. It only paces predictive cell
    // preload scheduling after an already-slow frame. Required cell/terrain/
    // object/groundcover rendering is never skipped or replaced with an empty
    // page, so enabling it cannot intentionally reduce visual coverage.
    inline bool adaptiveStreamingEnabled()
    {
        const std::string value = cells().mV3StreamingScheduler;
        return value == "adaptive";
    }

    inline float streamingTargetFrameMs()
    {
        return static_cast<float>(cells().mV3StreamingTargetFrametime);
    }
}

#endif
