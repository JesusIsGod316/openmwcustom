import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.20 Lua match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.20 Lua patched {rel} ({count} match(es))")


stats_path = ROOT / "components/debug/v320luafastpath.hpp"
if stats_path.exists():
    raise RuntimeError("components/debug/v320luafastpath.hpp already exists before CP2")
stats_path.write_text(
    r'''#ifndef OPENMW_COMPONENTS_DEBUG_V320LUAFASTPATH_H
#define OPENMW_COMPONENTS_DEBUG_V320LUAFASTPATH_H

#include <atomic>
#include <cstdint>

namespace Debug::V320LuaFastPath
{
    struct Counters
    {
        std::atomic<std::uint64_t> mEventChecks{ 0 };
        std::atomic<std::uint64_t> mNoHandlerFastPaths{ 0 };
        std::atomic<std::uint64_t> mActualDispatches{ 0 };
        std::atomic<std::uint64_t> mSoundConversionHits{ 0 };
        std::atomic<std::uint64_t> mSoundConversionMisses{ 0 };
        std::atomic<std::uint64_t> mSoundConversionEvictions{ 0 };
    };

    inline Counters& counters()
    {
        static Counters value;
        return value;
    }

    inline void recordEventCheck(bool hasHandlers)
    {
        Counters& value = counters();
        value.mEventChecks.fetch_add(1, std::memory_order_relaxed);
        if (!hasHandlers)
            value.mNoHandlerFastPaths.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordDispatch()
    {
        counters().mActualDispatches.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundHit()
    {
        counters().mSoundConversionHits.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundMiss()
    {
        counters().mSoundConversionMisses.fetch_add(1, std::memory_order_relaxed);
    }

    inline void recordSoundEviction()
    {
        counters().mSoundConversionEvictions.fetch_add(1, std::memory_order_relaxed);
    }
}

#endif
''',
    encoding="utf-8",
    newline="\n",
)
print("V3.20 Lua added components/debug/v320luafastpath.hpp")

lua_setting_anchor = '''        SettingValue<bool> mV33IdleTimerFastPath{ mIndex, "Lua", "v3.3 idle timer fast path" };'''
replace_exact(
    "components/settings/categories/lua.hpp",
    lua_setting_anchor,
    lua_setting_anchor
    + '''
        // V3.20 stock-semantic handler and pure-conversion fast paths.
        SettingValue<bool> mV320EngineLuaFastPaths{ mIndex, "Lua", "v3.20 engine event fast paths" };
        SettingValue<bool> mV320SoundConversionCache{ mIndex, "Lua", "v3.20 sound conversion cache" };''',
)

lua_default_anchor = "v3.3 idle timer fast path = false"
replace_exact(
    "files/settings-default.cfg",
    lua_default_anchor,
    lua_default_anchor
    + '''

# V3.20 stock-semantic engine event recipient checks and bounded TLS caching of
# deterministic sound ID/path conversions. These never cache handlers, resources,
# SoundBuffer/OpenAL objects, mutable world objects, or missing-resource results.
v3.20 engine event fast paths = true
v3.20 sound conversion cache = true''',
)

engine_include_anchor = "#include <components/debug/v33luatrace.hpp>\n"
replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    engine_include_anchor,
    engine_include_anchor + "#include <components/debug/v320luafastpath.hpp>\n",
)

old_engine_gate = '''        bool v317EngineFastPathEnabled()
        {
            // Launcher selection happens before process start, so this is immutable
            // for the lifetime of the process and cheap after the first query.
            static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;
            return enabled;
        }'''
new_engine_gate = '''        bool v317EngineFastPathEnabled()
        {
            // V3.20 promotes the mature V3.17 path to a native setting while preserving
            // the old environment switch and adding a fail-closed causal override.
            static const bool enabled = [] {
                const bool configured = Settings::lua().mV320EngineLuaFastPaths;
                if (const char* value = std::getenv("OPENMW_V320_ENGINE_LUA_FASTPATHS"))
                    return *value != 0 ? std::atoi(value) != 0 : configured;
                return std::getenv("OPENMW_V317_LUA_OPT") != nullptr || configured;
            }();
            return enabled;
        }'''
replace_exact("apps/openmw/mwlua/engineevents.cpp", old_engine_gate, new_engine_gate)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''                const bool globalHandlers = mGlobalScripts.hasOnActivateHandlers();
                const bool localHandlers = scripts && scripts->hasOnActivatedHandlers();
                if (!globalHandlers && !localHandlers)''',
    '''                const bool globalHandlers = mGlobalScripts.hasOnActivateHandlers();
                const bool localHandlers = scripts && scripts->hasOnActivatedHandlers();
                Debug::V320LuaFastPath::recordEventCheck(globalHandlers || localHandlers);
                if (!globalHandlers && !localHandlers)''',
)
replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''                if (globalHandlers)
                    mGlobalScripts.onActivate(GObject(obj), GObject(actor));
                if (localHandlers)
                    scripts->onActivated(LObject(actor));''',
    '''                if (globalHandlers)
                {
                    Debug::V320LuaFastPath::recordDispatch();
                    mGlobalScripts.onActivate(GObject(obj), GObject(actor));
                }
                if (localHandlers)
                {
                    Debug::V320LuaFastPath::recordDispatch();
                    scripts->onActivated(LObject(actor));
                }''',
)

for method, query in (
    ("OnUseItem", "mGlobalScripts.hasOnUseItemHandlers()"),
    ("OnDropped", "mGlobalScripts.hasOnDroppedHandlers()"),
    ("OnPlaced", "mGlobalScripts.hasOnPlacedHandlers()"),
):
    old = f'''            if (v317EngineFastPathEnabled() && !{query})
            {{'''
    new = f'''            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || {query};
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {{'''
    replace_exact("apps/openmw/mwlua/engineevents.cpp", old, new)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''            if (v317EngineFastPathEnabled() && (!scripts || !scripts->hasOnConsumeHandlers()))
            {''',
    '''            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || (scripts && scripts->hasOnConsumeHandlers());
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''            if (!v317EngineFastPathEnabled() || mGlobalScripts.hasOnNewExteriorHandlers())
                mGlobalScripts.onNewExterior(GCell{ &event.mCell });''',
    '''            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || mGlobalScripts.hasOnNewExteriorHandlers();
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (v320HasHandlers)
            {
                Debug::V320LuaFastPath::recordDispatch();
                mGlobalScripts.onNewExterior(GCell{ &event.mCell });
            }''',
)

for call in (
    "mGlobalScripts.onUseItem(GObject(obj), GObject(actor), event.mForce);",
    "mGlobalScripts.onDropped(GObject(obj), GObject(actor), event.mPosition, event.mRotation);",
    "mGlobalScripts.onPlaced(GObject(obj), GObject(actor), event.mPosition, event.mRotation);",
):
    replace_exact(
        "apps/openmw/mwlua/engineevents.cpp",
        "            " + call,
        "            Debug::V320LuaFastPath::recordDispatch();\n            " + call,
    )

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''            if (scripts)
                scripts->onConsume(LObject(consumable));''',
    '''            if (scripts)
            {
                Debug::V320LuaFastPath::recordDispatch();
                scripts->onConsume(LObject(consumable));
            }''',
)

sound_include_anchor = "#include <components/esm3/loadsoun.hpp>\n"
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    sound_include_anchor,
    sound_include_anchor + "#include <components/debug/v320luafastpath.hpp>\n",
)

old_sound_gate = '''    bool v317SoundBindingCacheEnabled()
    {
        static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;
        return enabled;
    }'''
new_sound_gate = '''    bool v317SoundBindingCacheEnabled()
    {
        static const bool enabled = [] {
            const bool configured = Settings::lua().mV320SoundConversionCache;
            if (const char* value = std::getenv("OPENMW_V320_SOUND_CONVERSION_CACHE"))
                return *value != 0 ? std::atoi(value) != 0 : configured;
            return std::getenv("OPENMW_V317_LUA_OPT") != nullptr || configured;
        }();
        return enabled;
    }'''
replace_exact("apps/openmw/mwlua/soundbindings.cpp", old_sound_gate, new_sound_gate)

old_sound_method = '''        ESM::RefId soundId(std::string_view value)
        {
            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)
                return ESM::RefId::deserializeText(value);
            if (const auto it = mSoundIds.find(value); it != mSoundIds.end())
                return it->second;
            ESM::RefId parsed = ESM::RefId::deserializeText(value);
            if (mSoundIds.size() < sMaxEntries)
                mSoundIds.emplace(std::string(value), parsed);
            return parsed;
        }'''
new_sound_method = '''        ESM::RefId soundId(std::string_view value)
        {
            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)
                return ESM::RefId::deserializeText(value);
            if (const auto it = mSoundIds.find(value); it != mSoundIds.end())
            {
                Debug::V320LuaFastPath::recordSoundHit();
                return it->second;
            }
            Debug::V320LuaFastPath::recordSoundMiss();
            ESM::RefId parsed = ESM::RefId::deserializeText(value);
            if (mSoundIds.size() >= sMaxEntries)
            {
                mSoundIds.erase(mSoundIds.begin());
                Debug::V320LuaFastPath::recordSoundEviction();
            }
            mSoundIds.emplace(std::string(value), parsed);
            return parsed;
        }'''
replace_exact("apps/openmw/mwlua/soundbindings.cpp", old_sound_method, new_sound_method)

old_path_method = '''        VFS::Path::Normalized path(std::string_view value)
        {
            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)
                return VFS::Path::Normalized(value);
            if (const auto it = mPaths.find(value); it != mPaths.end())
                return it->second;
            VFS::Path::Normalized parsed(value);
            if (mPaths.size() < sMaxEntries)
                mPaths.emplace(std::string(value), parsed);
            return parsed;
        }'''
new_path_method = '''        VFS::Path::Normalized path(std::string_view value)
        {
            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)
                return VFS::Path::Normalized(value);
            if (const auto it = mPaths.find(value); it != mPaths.end())
            {
                Debug::V320LuaFastPath::recordSoundHit();
                return it->second;
            }
            Debug::V320LuaFastPath::recordSoundMiss();
            VFS::Path::Normalized parsed(value);
            if (mPaths.size() >= sMaxEntries)
            {
                mPaths.erase(mPaths.begin());
                Debug::V320LuaFastPath::recordSoundEviction();
            }
            mPaths.emplace(std::string(value), parsed);
            return parsed;
        }'''
replace_exact("apps/openmw/mwlua/soundbindings.cpp", old_path_method, new_path_method)

manager_include_anchor = "#include <components/debug/v3diagnostics.hpp>\n"
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    manager_include_anchor,
    manager_include_anchor + "#include <components/debug/v320luafastpath.hpp>\n",
)

old_report = '''    void LuaManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Lua UsedMemory", static_cast<double>(mLua.getTotalMemoryUsage()));
    }'''
new_report = '''    void LuaManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "Lua UsedMemory", static_cast<double>(mLua.getTotalMemoryUsage()));
        const Debug::V320LuaFastPath::Counters& counters = Debug::V320LuaFastPath::counters();
        stats.setAttribute(frameNumber, "V320 Lua EventChecks",
            static_cast<double>(counters.mEventChecks.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua NoHandlerFastPaths",
            static_cast<double>(counters.mNoHandlerFastPaths.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua ActualDispatches",
            static_cast<double>(counters.mActualDispatches.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundConversionHits",
            static_cast<double>(counters.mSoundConversionHits.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundConversionMisses",
            static_cast<double>(counters.mSoundConversionMisses.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundConversionEvictions",
            static_cast<double>(counters.mSoundConversionEvictions.load(std::memory_order_relaxed)));
    }'''
replace_exact("apps/openmw/mwlua/luamanagerimp.cpp", old_report, new_report)

identity_anchor = "openmw-custom-v3.20-cp1-focus"
replace_exact("apps/openmw/engine.cpp", identity_anchor, identity_anchor + " / openmw-custom-v3.20-cp2-lua")

for rel, required in {
    "components/settings/categories/lua.hpp": ("mV320EngineLuaFastPaths", "mV320SoundConversionCache"),
    "files/settings-default.cfg": (
        "v3.20 engine event fast paths = true",
        "v3.20 sound conversion cache = true",
    ),
    "apps/openmw/mwlua/engineevents.cpp": (
        "OPENMW_V320_ENGINE_LUA_FASTPATHS",
        "recordEventCheck",
        "recordDispatch",
    ),
    "apps/openmw/mwlua/soundbindings.cpp": (
        "OPENMW_V320_SOUND_CONVERSION_CACHE",
        "recordSoundHit",
        "recordSoundMiss",
        "recordSoundEviction",
    ),
    "apps/openmw/mwlua/luamanagerimp.cpp": (
        "V320 Lua EventChecks",
        "V320 Lua SoundConversionEvictions",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"V3.20 Lua source missing {marker!r} in {rel}")

print("V3.20 CP2 engine-Lua and pure sound-conversion fast paths applied")
