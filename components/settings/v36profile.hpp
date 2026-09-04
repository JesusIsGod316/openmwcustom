#ifndef OPENMW_COMPONENTS_SETTINGS_V36PROFILE_H
#define OPENMW_COMPONENTS_SETTINGS_V36PROFILE_H

#include "values.hpp"

namespace Settings::V36Profile
{
    inline bool enabled()
    {
        return static_cast<bool>(cells().mV36PerformanceProfile);
    }

    inline bool ramOverdriveEnabled()
    {
        if (enabled())
            return !static_cast<bool>(cells().mV36DisableRamOverdrive);
        return false;
    }

    inline bool luaFastPathEnabled()
    {
        if (enabled())
            return !static_cast<bool>(cells().mV36DisableLuaFastPath);
        return static_cast<bool>(lua().mV33IdleTimerFastPath);
    }

    inline bool coarseChunkOcclusionEnabled()
    {
        if (enabled())
            return !static_cast<bool>(cells().mV36DisableCoarseChunkOcclusion);
        return static_cast<bool>(camera().mV35CoarseChunkOcclusion);
    }

    inline float farCasterMinimumPixels()
    {
        // V3.6/6144 A-B testing validated 2 px with no user-visible artifacts.
        // V3.8 extends only the farthest cascade with opt-in graded thresholds.
        if (enabled())
        {
            if (static_cast<bool>(cells().mV37DisableFarCasterPruning))
                return 0.f;

            switch (static_cast<int>(cells().mV38FarShadowMode))
            {
                case 1:
                    return 2.5f;
                case 2:
                    return 3.5f;
                case 3:
                    return 5.f;
                default:
                    return 2.f;
            }
        }
        return static_cast<float>(cells().mV36FarCasterMinimumPixels);
    }
}

#endif
