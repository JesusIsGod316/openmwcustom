import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.20 sound-query match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.20 sound-query patched {rel} ({count} match(es))")


setting_anchor = '''        SettingValue<bool> mV320SoundConversionCache{ mIndex, "Lua", "v3.20 sound conversion cache" };'''
replace_exact(
    "components/settings/categories/lua.hpp",
    setting_anchor,
    setting_anchor
    + '''
        SettingValue<bool> mV320SoundQueryCoalescing{ mIndex, "Lua", "v3.20 sound query coalescing" };''',
)

default_anchor = "v3.20 sound conversion cache = true"
replace_exact(
    "files/settings-default.cfg",
    default_anchor,
    default_anchor
    + '''
# Experimental: coalesce identical isSoundPlaying queries only within one engine
# frame. Any Lua sound play/stop mutation invalidates the frame-local results.
v3.20 sound query coalescing = false''',
)

counter_anchor = '''        std::atomic<std::uint64_t> mSoundConversionEvictions{ 0 };'''
replace_exact(
    "components/debug/v320luafastpath.hpp",
    counter_anchor,
    counter_anchor
    + '''
        std::atomic<std::uint64_t> mSoundQueryCalls{ 0 };
        std::atomic<std::uint64_t> mSoundQueryExecuted{ 0 };
        std::atomic<std::uint64_t> mSoundQueryCoalesced{ 0 };
        std::atomic<std::uint64_t> mSoundQueryDirtyInvalidations{ 0 };''',
)

include_anchor = "#include <components/debug/v320luafastpath.hpp>\n"
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    include_anchor,
    include_anchor + "#include <components/debug/v3hitchtelemetry.hpp>\n",
)
replace_exact("apps/openmw/mwlua/soundbindings.cpp", "#include <string>\n", "#include <string>\n#include <tuple>\n")

cache_anchor = '''    V317SoundBindingCache& v317SoundBindingCache()
    {
        thread_local V317SoundBindingCache cache;
        return cache;
    }
'''
cache_code = r'''
    bool v320SoundQueryCoalescingEnabled()
    {
        static const bool enabled = [] {
            const bool configured = Settings::lua().mV320SoundQueryCoalescing;
            if (const char* value = std::getenv("OPENMW_V320_SOUND_QUERY_COALESCING"))
                return *value != 0 ? std::atoi(value) != 0 : configured;
            return configured;
        }();
        return enabled;
    }

    struct V320SoundQueryKey
    {
        ESM::RefNum mObject;
        bool mFile = false;
        std::string mValue;

        friend bool operator<(const V320SoundQueryKey& left, const V320SoundQueryKey& right)
        {
            return std::tie(left.mObject, left.mFile, left.mValue)
                < std::tie(right.mObject, right.mFile, right.mValue);
        }
    };

    class V320SoundQueryCache
    {
    public:
        void invalidate()
        {
            if (!v320SoundQueryCoalescingEnabled())
                return;
            if (!mResults.empty())
                Debug::V320LuaFastPath::counters().mSoundQueryDirtyInvalidations.fetch_add(
                    1, std::memory_order_relaxed);
            mResults.clear();
        }

        bool soundId(const MWWorld::Ptr& ptr, std::string_view key, ESM::RefId sound)
        {
            return query(ptr, false, key, [&] {
                return MWBase::Environment::get().getSoundManager()->getSoundPlaying(ptr, sound);
            });
        }

        bool path(const MWWorld::Ptr& ptr, std::string_view key, const VFS::Path::Normalized& path)
        {
            return query(ptr, true, key, [&] {
                return MWBase::Environment::get().getSoundManager()->getSoundPlaying(ptr, path);
            });
        }

    private:
        template <class Query>
        bool query(const MWWorld::Ptr& ptr, bool file, std::string_view value, Query&& execute)
        {
            Debug::V320LuaFastPath::Counters& counters = Debug::V320LuaFastPath::counters();
            counters.mSoundQueryCalls.fetch_add(1, std::memory_order_relaxed);
            if (!v320SoundQueryCoalescingEnabled())
            {
                counters.mSoundQueryExecuted.fetch_add(1, std::memory_order_relaxed);
                return execute();
            }

            const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
            if (frame != mFrame)
            {
                mResults.clear();
                mFrame = frame;
            }
            const ESM::RefNum object = ptr.isEmpty() ? ESM::RefNum{} : ptr.getCellRef().getRefNum();
            V320SoundQueryKey key{ object, file, std::string(value) };
            if (const auto it = mResults.find(key); it != mResults.end())
            {
                counters.mSoundQueryCoalesced.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }

            counters.mSoundQueryExecuted.fetch_add(1, std::memory_order_relaxed);
            const bool result = execute();
            if (mResults.size() >= sMaxEntries)
                mResults.erase(mResults.begin());
            mResults.emplace(std::move(key), result);
            return result;
        }

        static constexpr std::size_t sMaxEntries = 4096;
        unsigned mFrame = std::numeric_limits<unsigned>::max();
        std::map<V320SoundQueryKey, bool> mResults;
    };

    V320SoundQueryCache& v320SoundQueryCache()
    {
        thread_local V320SoundQueryCache cache;
        return cache;
    }
'''
replace_exact("apps/openmw/mwlua/soundbindings.cpp", cache_anchor, cache_anchor + cache_code)
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    "#include <map>\n",
    "#include <limits>\n#include <map>\n",
)

for old in (
    "            MWBase::Environment::get().getSoundManager()->playSound(\n",
    "            MWBase::Environment::get().getSoundManager()->playSound(v317SoundBindingCache().path(fileName), args.mVolume,\n",
    "            MWBase::Environment::get().getSoundManager()->stopSound3D(MWWorld::Ptr(), sound);",
    "            MWBase::Environment::get().getSoundManager()->stopSound3D(MWWorld::Ptr(), v317SoundBindingCache().path(fileName));",
    "                  MWBase::Environment::get().getSoundManager()->playSound3D(\n",
    "                  MWBase::Environment::get().getSoundManager()->playSound3D(ptr, v317SoundBindingCache().path(fileName),\n",
    "            MWBase::Environment::get().getSoundManager()->stopSound3D(ptr, sound);",
    "            MWBase::Environment::get().getSoundManager()->stopSound3D(ptr, v317SoundBindingCache().path(fileName));",
):
    indent = old[: len(old) - len(old.lstrip())]
    replace_exact(
        "apps/openmw/mwlua/soundbindings.cpp",
        old,
        indent + "v320SoundQueryCache().invalidate();\n" + old,
    )

replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''            return MWBase::Environment::get().getSoundManager()->getSoundPlaying(MWWorld::Ptr(), sound);''',
    '''            return v320SoundQueryCache().soundId(MWWorld::Ptr(), soundId, sound);''',
)
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''            return MWBase::Environment::get().getSoundManager()->getSoundPlaying(
                MWWorld::Ptr(), v317SoundBindingCache().path(fileName));''',
    '''            const VFS::Path::Normalized path = v317SoundBindingCache().path(fileName);
            return v320SoundQueryCache().path(MWWorld::Ptr(), fileName, path);''',
)
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''        api["isSoundPlaying"] = [](std::string_view soundId, const sol::object& object) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            const MWWorld::Ptr& ptr = getPtrOrThrow(ObjectVariant(object));
            return MWBase::Environment::get().getSoundManager()->getSoundPlaying(ptr, sound);
        };''',
    '''        api["isSoundPlaying"] = [](std::string_view soundId, const sol::object& object) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            const MWWorld::Ptr& ptr = getPtrOrThrow(ObjectVariant(object));
            return v320SoundQueryCache().soundId(ptr, soundId, sound);
        };''',
)
replace_exact(
    "apps/openmw/mwlua/soundbindings.cpp",
    '''            return MWBase::Environment::get().getSoundManager()->getSoundPlaying(ptr, v317SoundBindingCache().path(fileName));''',
    '''            const VFS::Path::Normalized path = v317SoundBindingCache().path(fileName);
            return v320SoundQueryCache().path(ptr, fileName, path);''',
)

report_anchor = '''        stats.setAttribute(frameNumber, "V320 Lua SoundConversionEvictions",
            static_cast<double>(counters.mSoundConversionEvictions.load(std::memory_order_relaxed)));'''
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    report_anchor,
    report_anchor
    + '''
        stats.setAttribute(frameNumber, "V320 Lua SoundQueryCalls",
            static_cast<double>(counters.mSoundQueryCalls.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundQueryExecuted",
            static_cast<double>(counters.mSoundQueryExecuted.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundQueryCoalesced",
            static_cast<double>(counters.mSoundQueryCoalesced.load(std::memory_order_relaxed)));
        stats.setAttribute(frameNumber, "V320 Lua SoundQueryDirtyInvalidations",
            static_cast<double>(counters.mSoundQueryDirtyInvalidations.load(std::memory_order_relaxed)));''',
)

identity_anchor = "openmw-custom-v3.20-cp2-lua"
replace_exact("apps/openmw/engine.cpp", identity_anchor, identity_anchor + " / openmw-custom-v3.20-cp3-sound-query")

scene_text = (ROOT / "components/resource/scenemanager.cpp").read_text(encoding="utf-8")
cache_marker = "mCache->getRefFromObjectCache(path)"
miss_marker = 'TraceScope trace("render", "scene_template_miss"'
if cache_marker not in scene_text or miss_marker not in scene_text or scene_text.index(cache_marker) > scene_text.index(miss_marker):
    raise RuntimeError("V3.20 CP3 scene-template positive-cache audit invariant failed")

for rel, markers in {
    "components/settings/categories/lua.hpp": ("mV320SoundQueryCoalescing",),
    "files/settings-default.cfg": ("v3.20 sound query coalescing = false",),
    "apps/openmw/mwlua/soundbindings.cpp": (
        "OPENMW_V320_SOUND_QUERY_COALESCING",
        "mSoundQueryCoalesced",
        "v320SoundQueryCache().invalidate()",
    ),
    "apps/openmw/mwlua/luamanagerimp.cpp": ("V320 Lua SoundQueryDirtyInvalidations",),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.20 CP3 generated source missing {marker!r} in {rel}")

print("V3.20 CP3 same-frame sound-query coalescing applied; redundant template cache rejected")
