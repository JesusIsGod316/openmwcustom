import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.17 engine-Lua match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.17 engine-Lua patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Safe handler-presence query.
#
# The important semantic rule is that an UNLOADED container is always treated as
# "may have handlers". We do not persist negative handler-interest information
# across unload/reload because arbitrary Lua top-level code can legally choose a
# different handler table when it is materialized again. The optimization only
# skips argument/object construction for a container that is already loaded and
# whose current handler list is known empty.
# -----------------------------------------------------------------------------
replace_exact(
    "components/lua/scriptscontainer.hpp",
    '''        // Calls given handlers in direct order.\n        template <typename... Args>\n        void callEngineHandlers(EngineHandlerList& handlers, const Args&... args)\n        {\n            if (handlers.mList.empty() && std::holds_alternative<LoadedData>(mData))\n                return;''',
    '''        // V3.17: callers that would otherwise construct Lua wrapper arguments can\n        // cheaply test a handler list first. Unloaded containers deliberately return\n        // true because materialization may legally produce a different handler table.\n        bool mightHaveEngineHandlers(const EngineHandlerList& handlers) const\n        {\n            return !handlers.mList.empty() || std::holds_alternative<UnloadedData>(mData);\n        }\n\n        // Calls given handlers in direct order.\n        template <typename... Args>\n        void callEngineHandlers(EngineHandlerList& handlers, const Args&... args)\n        {\n            if (handlers.mList.empty() && std::holds_alternative<LoadedData>(mData))\n                return;''',
)


# -----------------------------------------------------------------------------
# Expose conservative "might handle" checks for global and local event lanes.
# V3.7 already added active-event checks; upgrade those to the same unload-safe
# contract and extend it to event paths that otherwise build GObject/LObject
# wrappers before discovering that no Lua callback exists.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwlua/globalscripts.hpp",
    '''        bool hasObjectActiveHandlers() const { return !mObjectActiveHandlers.mList.empty(); }\n        bool hasActorActiveHandlers() const { return !mActorActiveHandlers.mList.empty(); }\n        bool hasItemActiveHandlers() const { return !mItemActiveHandlers.mList.empty(); }\n        bool hasPlayerAddedHandlers() const { return !mPlayerAddedHandlers.mList.empty(); }''',
    '''        bool hasObjectActiveHandlers() const { return mightHaveEngineHandlers(mObjectActiveHandlers); }\n        bool hasActorActiveHandlers() const { return mightHaveEngineHandlers(mActorActiveHandlers); }\n        bool hasItemActiveHandlers() const { return mightHaveEngineHandlers(mItemActiveHandlers); }\n        bool hasPlayerAddedHandlers() const { return mightHaveEngineHandlers(mPlayerAddedHandlers); }\n        bool hasOnActivateHandlers() const { return mightHaveEngineHandlers(mOnActivateHandlers); }\n        bool hasOnUseItemHandlers() const { return mightHaveEngineHandlers(mOnUseItemHandlers); }\n        bool hasOnDroppedHandlers() const { return mightHaveEngineHandlers(mOnDroppedHandlers); }\n        bool hasOnPlacedHandlers() const { return mightHaveEngineHandlers(mOnPlacedHandlers); }\n        bool hasOnNewExteriorHandlers() const { return mightHaveEngineHandlers(mOnNewExteriorHandlers); }''',
)

replace_exact(
    "apps/openmw/mwlua/localscripts.hpp",
    '''        bool isActive() const override { return mData.mIsActive; }\n        void onConsume(const LObject& consumable) { callEngineHandlers(mOnConsumeHandlers, consumable); }\n        void onActivated(const LObject& actor) { callEngineHandlers(mOnActivatedHandlers, actor); }''',
    '''        bool isActive() const override { return mData.mIsActive; }\n        bool hasOnConsumeHandlers() const { return mightHaveEngineHandlers(mOnConsumeHandlers); }\n        bool hasOnActivatedHandlers() const { return mightHaveEngineHandlers(mOnActivatedHandlers); }\n        void onConsume(const LObject& consumable) { callEngineHandlers(mOnConsumeHandlers, consumable); }\n        void onActivated(const LObject& actor) { callEngineHandlers(mOnActivatedHandlers, actor); }''',
)


# -----------------------------------------------------------------------------
# Engine event fast paths.
#
# Mode90/91 controls must remain byte-for-byte behaviorally conservative at
# runtime, so the optimized branch is selected only by OPENMW_V317_LUA_OPT.
# When Lua debug logging is enabled we still resolve otherwise-skipped RefNums so
# missing-object diagnostics are preserved.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''#include "engineevents.hpp"\n\n#include <components/debug/debuglog.hpp>''',
    '''#include "engineevents.hpp"\n\n#include <cstdlib>\n\n#include <components/debug/debuglog.hpp>''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''namespace MWLua\n{\n\n    class EngineEvents::Visitor''',
    '''namespace MWLua\n{\n    namespace\n    {\n        bool v317EngineFastPathEnabled()\n        {\n            // Launcher selection happens before process start, so this is immutable\n            // for the lifetime of the process and cheap after the first query.\n            static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;\n            return enabled;\n        }\n    }\n\n    class EngineEvents::Visitor''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''        void operator()(const OnActivate& event) const\n        {\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (actor.isEmpty() || obj.isEmpty())\n                return;\n            mGlobalScripts.onActivate(GObject(obj), GObject(actor));\n            if (auto* scripts = getLocalScripts(obj))\n                scripts->onActivated(LObject(actor));\n        }''',
    '''        void operator()(const OnActivate& event) const\n        {\n            if (v317EngineFastPathEnabled())\n            {\n                MWWorld::Ptr obj = getPtr(event.mObject);\n                if (obj.isEmpty())\n                    return;\n                LocalScripts* scripts = getLocalScripts(obj);\n                const bool globalHandlers = mGlobalScripts.hasOnActivateHandlers();\n                const bool localHandlers = scripts && scripts->hasOnActivatedHandlers();\n                if (!globalHandlers && !localHandlers)\n                {\n                    if (Settings::lua().mLuaDebug)\n                        (void)getPtr(event.mActor);\n                    return;\n                }\n                MWWorld::Ptr actor = getPtr(event.mActor);\n                if (actor.isEmpty())\n                    return;\n                if (globalHandlers)\n                    mGlobalScripts.onActivate(GObject(obj), GObject(actor));\n                if (localHandlers)\n                    scripts->onActivated(LObject(actor));\n                return;\n            }\n\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (actor.isEmpty() || obj.isEmpty())\n                return;\n            mGlobalScripts.onActivate(GObject(obj), GObject(actor));\n            if (auto* scripts = getLocalScripts(obj))\n                scripts->onActivated(LObject(actor));\n        }''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''        void operator()(const OnUseItem& event) const\n        {\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (actor.isEmpty() || obj.isEmpty())\n                return;\n            mGlobalScripts.onUseItem(GObject(obj), GObject(actor), event.mForce);\n        }''',
    '''        void operator()(const OnUseItem& event) const\n        {\n            if (v317EngineFastPathEnabled() && !mGlobalScripts.hasOnUseItemHandlers())\n            {\n                if (Settings::lua().mLuaDebug)\n                {\n                    (void)getPtr(event.mObject);\n                    (void)getPtr(event.mActor);\n                }\n                return;\n            }\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (actor.isEmpty() || obj.isEmpty())\n                return;\n            mGlobalScripts.onUseItem(GObject(obj), GObject(actor), event.mForce);\n        }''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''        void operator()(const OnConsume& event) const\n        {\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            MWWorld::Ptr consumable = getPtr(event.mConsumable);\n            if (actor.isEmpty() || consumable.isEmpty())\n                return;\n            if (auto* scripts = getLocalScripts(actor))\n                scripts->onConsume(LObject(consumable));\n        }''',
    '''        void operator()(const OnConsume& event) const\n        {\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (actor.isEmpty())\n                return;\n            LocalScripts* scripts = getLocalScripts(actor);\n            if (v317EngineFastPathEnabled() && (!scripts || !scripts->hasOnConsumeHandlers()))\n            {\n                if (Settings::lua().mLuaDebug)\n                    (void)getPtr(event.mConsumable);\n                return;\n            }\n            MWWorld::Ptr consumable = getPtr(event.mConsumable);\n            if (consumable.isEmpty())\n                return;\n            if (scripts)\n                scripts->onConsume(LObject(consumable));\n        }''',
)

for event_name, query_name, call_name in (
    ("OnDropped", "hasOnDroppedHandlers", "onDropped"),
    ("OnPlaced", "hasOnPlacedHandlers", "onPlaced"),
):
    old = f'''        void operator()(const {event_name}& event) const\n        {{\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (obj.isEmpty() || actor.isEmpty())\n                return;\n            mGlobalScripts.{call_name}(GObject(obj), GObject(actor), event.mPosition, event.mRotation);\n        }}'''
    new = f'''        void operator()(const {event_name}& event) const\n        {{\n            if (v317EngineFastPathEnabled() && !mGlobalScripts.{query_name}())\n            {{\n                if (Settings::lua().mLuaDebug)\n                {{\n                    (void)getPtr(event.mObject);\n                    (void)getPtr(event.mActor);\n                }}\n                return;\n            }}\n            MWWorld::Ptr obj = getPtr(event.mObject);\n            MWWorld::Ptr actor = getPtr(event.mActor);\n            if (obj.isEmpty() || actor.isEmpty())\n                return;\n            mGlobalScripts.{call_name}(GObject(obj), GObject(actor), event.mPosition, event.mRotation);\n        }}'''
    replace_exact("apps/openmw/mwlua/engineevents.cpp", old, new)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''        void operator()(const OnNewExterior& event) const { mGlobalScripts.onNewExterior(GCell{ &event.mCell }); }''',
    '''        void operator()(const OnNewExterior& event) const\n        {\n            if (!v317EngineFastPathEnabled() || mGlobalScripts.hasOnNewExteriorHandlers())\n                mGlobalScripts.onNewExterior(GCell{ &event.mCell });\n        }''',
)


# -----------------------------------------------------------------------------
# Lua -> sound conversion cache.
#
# Dynamic-sound mods can cross this binding many times per second with the same
# sound IDs and normalized file names. RefId text decoding and VFS path
# normalization are immutable pure conversions, so cache them per calling thread.
# The cache is bounded and refuses oversized keys; it never delays sound start and
# never caches SoundBuffer/OpenAL state.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''#include <components/vfs/pathutil.hpp>\n\n#include "luamanagerimp.hpp"''',
    '''#include <components/vfs/pathutil.hpp>\n\n#include <cstdlib>\n#include <map>\n#include <string>\n\n#include "luamanagerimp.hpp"''',
)

replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''    struct StreamMusicArgs\n    {\n        float mFade = 1.f;\n    };\n\n    MWWorld::Ptr getMutablePtrOrThrow''',
    '''    struct StreamMusicArgs\n    {\n        float mFade = 1.f;\n    };\n\n    bool v317SoundBindingCacheEnabled()\n    {\n        static const bool enabled = std::getenv("OPENMW_V317_LUA_OPT") != nullptr;\n        return enabled;\n    }\n\n    class V317SoundBindingCache\n    {\n    public:\n        ESM::RefId soundId(std::string_view value)\n        {\n            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)\n                return ESM::RefId::deserializeText(value);\n            if (const auto it = mSoundIds.find(value); it != mSoundIds.end())\n                return it->second;\n            ESM::RefId parsed = ESM::RefId::deserializeText(value);\n            if (mSoundIds.size() < sMaxEntries)\n                mSoundIds.emplace(std::string(value), parsed);\n            return parsed;\n        }\n\n        VFS::Path::Normalized path(std::string_view value)\n        {\n            if (!v317SoundBindingCacheEnabled() || value.size() > sMaxKeyBytes)\n                return VFS::Path::Normalized(value);\n            if (const auto it = mPaths.find(value); it != mPaths.end())\n                return it->second;\n            VFS::Path::Normalized parsed(value);\n            if (mPaths.size() < sMaxEntries)\n                mPaths.emplace(std::string(value), parsed);\n            return parsed;\n        }\n\n    private:\n        static constexpr std::size_t sMaxEntries = 4096;\n        static constexpr std::size_t sMaxKeyBytes = 512;\n        std::map<std::string, ESM::RefId, std::less<>> mSoundIds;\n        std::map<std::string, VFS::Path::Normalized, std::less<>> mPaths;\n    };\n\n    V317SoundBindingCache& v317SoundBindingCache()\n    {\n        thread_local V317SoundBindingCache cache;\n        return cache;\n    }\n\n    MWWorld::Ptr getMutablePtrOrThrow''',
)

replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''ESM::RefId sound = ESM::RefId::deserializeText(soundId);''',
    '''ESM::RefId sound = v317SoundBindingCache().soundId(soundId);''',
    expected=6,
)
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''VFS::Path::Normalized(fileName)''',
    '''v317SoundBindingCache().path(fileName)''',
    expected=9,
)


# Generated-source fail-closed markers.
for rel, required in {
    "components/lua/scriptscontainer.hpp": (
        "mightHaveEngineHandlers",
        "std::holds_alternative<UnloadedData>(mData)",
    ),
    "apps/openmw/mwlua/globalscripts.hpp": (
        "hasOnUseItemHandlers",
        "hasOnDroppedHandlers",
        "hasOnPlacedHandlers",
    ),
    "apps/openmw/mwlua/localscripts.hpp": (
        "hasOnConsumeHandlers",
        "hasOnActivatedHandlers",
    ),
    "apps/openmw/mwlua/engineevents.cpp": (
        "v317EngineFastPathEnabled",
        "OPENMW_V317_LUA_OPT",
        "mGlobalScripts.hasOnUseItemHandlers()",
    ),
    "apps/openmw/mwlua/soundbindings.cpp": (
        "class V317SoundBindingCache",
        "sMaxEntries = 4096",
        "v317SoundBindingCache().soundId(soundId)",
        "v317SoundBindingCache().path(fileName)",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"{rel}: V3.17 generated fast-path marker missing: {marker}")

print("V3.17 engine Lua/event and Lua->sound fast paths applied successfully.")
