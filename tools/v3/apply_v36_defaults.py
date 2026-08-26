import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 defaults match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 defaults patched {rel} ({count} match(es))")


def write_new(rel, text):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: refusing to overwrite an existing file")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"V3.6 defaults added {rel}")


replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme", "overdrive" }) };''',
    '''        // V3.6 keeps profile controls in one category object while preserving the user-facing [V3] section.
        SettingValue<bool> mV36PerformanceProfile{ mIndex, "V3", "v3.6 performance profile" };
        SettingValue<bool> mV36DisableRamOverdrive{ mIndex, "V3", "v3.6 disable ram overdrive" };
        SettingValue<bool> mV36DisableLuaFastPath{ mIndex, "V3", "v3.6 disable lua fast path" };
        SettingValue<bool> mV36DisableCoarseChunkOcclusion{
            mIndex, "V3", "v3.6 disable coarse chunk occlusion" };
        SettingValue<bool> mV36AsyncGpuProfiler{ mIndex, "V3", "v3.6 async gpu profiler" };
        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "V3", "v3.6 far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };

        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme", "overdrive" }) };''',
)

write_new(
    "components/settings/v36profile.hpp",
    r'''#ifndef OPENMW_COMPONENTS_SETTINGS_V36PROFILE_H
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
}

#endif
''',
)

replace_exact(
    "components/settings/ramcache.hpp",
    '''#include "values.hpp"''',
    '''#include "v36profile.hpp"
#include "values.hpp"''',
)
replace_exact(
    "components/settings/ramcache.hpp",
    '''    inline Mode mode()
    {
        const std::string value = cells().mRamCacheMode;''',
    '''    inline Mode mode()
    {
        if (V36Profile::enabled())
            return V36Profile::ramOverdriveEnabled() ? Mode::Overdrive : Mode::Normal;
        const std::string value = cells().mRamCacheMode;''',
)
replace_exact(
    "components/settings/ramcache.hpp",
    '''    inline OverdrivePreload overdrivePreload()
    {
        const std::string value = cells().mRamCacheOverdrivePreload;''',
    '''    inline OverdrivePreload overdrivePreload()
    {
        if (V36Profile::enabled())
            return OverdrivePreload::Balanced;
        const std::string value = cells().mRamCacheOverdrivePreload;''',
)

replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/settings/v36profile.hpp>
#include <components/settings/values.hpp>''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        const bool v33IdleTimerFastPath = Settings::lua().mV33IdleTimerFastPath;''',
    '''        const bool v33IdleTimerFastPath = Settings::V36Profile::luaFastPathEnabled();''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include <components/settings/categories/water.hpp>''',
    '''#include <components/settings/categories/water.hpp>
#include <components/settings/v36profile.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''Settings::camera().mV35CoarseChunkOcclusion''',
    '''Settings::V36Profile::coarseChunkOcclusionEnabled()''',
    expected=4,
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''#include <components/settings/ramcache.hpp>''',
    '''#include <components/settings/ramcache.hpp>
#include <components/settings/v36profile.hpp>''',
)
replace_exact(
    "apps/openmw/engine.cpp",
    '''                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName()''',
    '''                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName()
                     << " v3.6 profile=" << (Settings::V36Profile::enabled() ? "on" : "off")
                     << " lua-fast=" << (Settings::V36Profile::luaFastPathEnabled() ? "on" : "off")
                     << " coarse-msoc="
                     << (Settings::V36Profile::coarseChunkOcclusionEnabled() ? "on" : "off")''',
)

replace_exact(
    "files/settings-default.cfg",
    '''[Cells]

# V3 RAM/cache policy preset.''',
    '''[V3]

# V3.6 normal-play profile. This deliberately overrides stale V3.5 false values for the three proven optimizations.
# Set the profile false for legacy per-setting behavior, or use an individual disable switch for troubleshooting.
v3.6 performance profile = true
v3.6 disable ram overdrive = false
v3.6 disable lua fast path = false
v3.6 disable coarse chunk occlusion = false

# Diagnostics/experiments remain off unless explicitly selected.
v3.6 async gpu profiler = false
v3.6 far caster minimum pixels = 0.0

[Cells]

# V3 RAM/cache policy preset.''',
)

print("V3.6 proven-default profile source patch completed successfully.")
