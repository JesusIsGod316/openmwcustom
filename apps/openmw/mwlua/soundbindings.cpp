#include "soundbindings.hpp"
#include "recordstore.hpp"

#include "types/usertypeutil.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/esmstore.hpp"

#include <components/esm3/loadsoun.hpp>
#include <components/debug/v320luafastpath.hpp>
#include <components/debug/v3hitchtelemetry.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/pathutil.hpp>

#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <tuple>

#include "luamanagerimp.hpp"
#include "objectvariant.hpp"

namespace
{
    struct PlaySoundArgs
    {
        bool mScale = true;
        bool mLoop = false;
        float mVolume = 1.f;
        float mPitch = 1.f;
        float mTimeOffset = 0.f;
    };

    struct StreamMusicArgs
    {
        float mFade = 1.f;
    };

    bool v317SoundBindingCacheEnabled()
    {
        static const bool enabled = [] {
            const bool configured = Settings::lua().mV320SoundConversionCache;
            if (const char* value = std::getenv("OPENMW_V320_SOUND_CONVERSION_CACHE"))
                return *value != 0 ? std::atoi(value) != 0 : configured;
            return std::getenv("OPENMW_V317_LUA_OPT") != nullptr || configured;
        }();
        return enabled;
    }

    class V317SoundBindingCache
    {
    public:
        ESM::RefId soundId(std::string_view value)
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
        }

        VFS::Path::Normalized path(std::string_view value)
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
        }

    private:
        static constexpr std::size_t sMaxEntries = 4096;
        static constexpr std::size_t sMaxKeyBytes = 512;
        std::map<std::string, ESM::RefId, std::less<>> mSoundIds;
        std::map<std::string, VFS::Path::Normalized, std::less<>> mPaths;
    };

    V317SoundBindingCache& v317SoundBindingCache()
    {
        thread_local V317SoundBindingCache cache;
        return cache;
    }

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

    MWWorld::Ptr getMutablePtrOrThrow(const MWLua::ObjectVariant& variant)
    {
        if (variant.isLObject())
            throw std::runtime_error("Local scripts can only modify object they are attached to.");

        MWWorld::Ptr ptr = variant.ptr();
        if (ptr.isEmpty())
            throw std::runtime_error("Invalid object");

        return ptr;
    }

    MWWorld::Ptr getPtrOrThrow(const MWLua::ObjectVariant& variant)
    {
        MWWorld::Ptr ptr = variant.ptr();
        if (ptr.isEmpty())
            throw std::runtime_error("Invalid object");

        return ptr;
    }

    PlaySoundArgs getPlaySoundArgs(const sol::optional<sol::table>& options)
    {
        PlaySoundArgs args;

        if (options.has_value())
        {
            args.mLoop = options->get_or("loop", false);
            args.mVolume = options->get_or("volume", 1.f);
            args.mPitch = options->get_or("pitch", 1.f);
            args.mTimeOffset = options->get_or("timeOffset", 0.f);
            args.mScale = options->get_or("scale", true);
        }
        return args;
    }

    MWSound::PlayMode getPlayMode(const PlaySoundArgs& args, bool is3D)
    {
        if (is3D)
        {
            if (args.mLoop)
                return MWSound::PlayMode::LoopRemoveAtDistance;
            return MWSound::PlayMode::Normal;
        }

        if (args.mLoop && !args.mScale)
            return MWSound::PlayMode::LoopNoEnvNoScaling;
        else if (args.mLoop)
            return MWSound::PlayMode::LoopNoEnv;
        else if (!args.mScale)
            return MWSound::PlayMode::NoEnvNoScaling;
        return MWSound::PlayMode::NoEnv;
    }

    StreamMusicArgs getStreamMusicArgs(const sol::optional<sol::table>& options)
    {
        StreamMusicArgs args;

        if (options.has_value())
        {
            args.mFade = options->get_or("fadeOut", 1.f);
        }
        return args;
    }
}

namespace MWLua
{
    namespace
    {
        template <class T>
        void addUserType(sol::state_view& lua, std::string_view name)
        {
            sol::usertype<T> record = lua.new_usertype<T>(name);

            record[sol::meta_function::to_string]
                = [](const T& rec) -> std::string { return "ESM3_Sound[" + rec.mId.toDebugString() + "]"; };
            record["id"] = sol::readonly_property([](const T& rec) -> ESM::RefId { return rec.mId; });

            Types::addProperty(record, "volume", &ESM::Sound::mData, &ESM::SOUNstruct::mVolume);
            Types::addProperty(record, "minRange", &ESM::Sound::mData, &ESM::SOUNstruct::mMinRange);
            Types::addProperty(record, "maxRange", &ESM::Sound::mData, &ESM::SOUNstruct::mMaxRange);

            if constexpr (Types::RecordType<T>::isMutable)
            {
                record["fileName"] = sol::property(
                    [](const T& mutRec) -> std::string {
                        return Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(mutRec.find().mSound));
                    },
                    [](T& mutRec, std::string_view path) {
                        ESM::Sound& recordValue = mutRec.find();
                        recordValue.mSound = Misc::ResourceHelpers::soundPathForESM3(path);
                    });
            }
            else
            {
                record["fileName"] = sol::readonly_property([](const ESM::Sound& rec) -> std::string {
                    return Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(rec.mSound));
                });
            }
        }
    }

    sol::table initAmbientPackage(const Context& context)
    {
        sol::state_view lua = context.sol();
        if (lua["openmw_ambient"] != sol::nil)
            return lua["openmw_ambient"];

        sol::table api(lua, sol::create);

        api["playSound"] = [](std::string_view soundId, const sol::optional<sol::table>& options) {
            auto args = getPlaySoundArgs(options);
            auto playMode = getPlayMode(args, false);
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);

            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->playSound(
                sound, args.mVolume, args.mPitch, MWSound::Type::Sfx, playMode, args.mTimeOffset);
        };
        api["playSoundFile"] = [](std::string_view fileName, const sol::optional<sol::table>& options) {
            auto args = getPlaySoundArgs(options);
            auto playMode = getPlayMode(args, false);

            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->playSound(v317SoundBindingCache().path(fileName), args.mVolume,
                args.mPitch, MWSound::Type::Sfx, playMode, args.mTimeOffset);
        };

        api["stopSound"] = [](std::string_view soundId) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->stopSound3D(MWWorld::Ptr(), sound);
        };
        api["stopSoundFile"] = [](std::string_view fileName) {
            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->stopSound3D(MWWorld::Ptr(), v317SoundBindingCache().path(fileName));
        };

        api["isSoundPlaying"] = [](std::string_view soundId) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            return v320SoundQueryCache().soundId(MWWorld::Ptr(), soundId, sound);
        };
        api["isSoundFilePlaying"] = [](std::string_view fileName) {
            const VFS::Path::Normalized path = v317SoundBindingCache().path(fileName);
            return v320SoundQueryCache().path(MWWorld::Ptr(), fileName, path);
        };

        api["streamMusic"] = [](std::string_view fileName, const sol::optional<sol::table>& options) {
            auto args = getStreamMusicArgs(options);
            MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
            sndMgr->streamMusic(v317SoundBindingCache().path(fileName), MWSound::MusicType::Normal, args.mFade);
        };

        api["say"]
            = [luaManager = context.mLuaManager](std::string_view fileName, sol::optional<std::string_view> text) {
                  MWBase::Environment::get().getSoundManager()->say(v317SoundBindingCache().path(fileName));
                  if (text && Settings::gui().mSubtitles)
                      luaManager->addUIMessage(*text);
              };

        api["stopSay"] = []() { MWBase::Environment::get().getSoundManager()->stopSay(MWWorld::ConstPtr()); };
        api["isSayActive"]
            = []() { return MWBase::Environment::get().getSoundManager()->sayActive(MWWorld::ConstPtr()); };

        api["isMusicPlaying"] = []() { return MWBase::Environment::get().getSoundManager()->isMusicPlaying(); };

        api["stopMusic"] = []() {
            MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
            if (sndMgr->getMusicType() == MWSound::MusicType::MWScript)
                return;

            sndMgr->stopMusic();
        };

        lua["openmw_ambient"] = LuaUtil::makeReadOnly(api);
        return lua["openmw_ambient"];
    }

    sol::table initCoreSoundBindings(const Context& context)
    {
        sol::state_view lua = context.sol();
        sol::table api(lua, sol::create);

        api["isEnabled"] = []() { return MWBase::Environment::get().getSoundManager()->isEnabled(); };

        api["playSound3d"]
            = [](std::string_view soundId, const sol::object& object, const sol::optional<sol::table>& options) {
                  auto args = getPlaySoundArgs(options);
                  auto playMode = getPlayMode(args, true);

                  ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
                  MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));

                  v320SoundQueryCache().invalidate();
                  MWBase::Environment::get().getSoundManager()->playSound3D(
                      ptr, sound, args.mVolume, args.mPitch, MWSound::Type::Sfx, playMode, args.mTimeOffset);
              };
        api["playSoundFile3d"]
            = [](std::string_view fileName, const sol::object& object, const sol::optional<sol::table>& options) {
                  auto args = getPlaySoundArgs(options);
                  auto playMode = getPlayMode(args, true);
                  MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));

                  v320SoundQueryCache().invalidate();
                  MWBase::Environment::get().getSoundManager()->playSound3D(ptr, v317SoundBindingCache().path(fileName),
                      args.mVolume, args.mPitch, MWSound::Type::Sfx, playMode, args.mTimeOffset);
              };

        api["stopSound3d"] = [](std::string_view soundId, const sol::object& object) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));
            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->stopSound3D(ptr, sound);
        };
        api["stopSoundFile3d"] = [](std::string_view fileName, const sol::object& object) {
            MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));
            v320SoundQueryCache().invalidate();
            MWBase::Environment::get().getSoundManager()->stopSound3D(ptr, v317SoundBindingCache().path(fileName));
        };

        api["isSoundPlaying"] = [](std::string_view soundId, const sol::object& object) {
            ESM::RefId sound = v317SoundBindingCache().soundId(soundId);
            const MWWorld::Ptr& ptr = getPtrOrThrow(ObjectVariant(object));
            return v320SoundQueryCache().soundId(ptr, soundId, sound);
        };
        api["isSoundFilePlaying"] = [](std::string_view fileName, const sol::object& object) {
            const MWWorld::Ptr& ptr = getPtrOrThrow(ObjectVariant(object));
            const VFS::Path::Normalized path = v317SoundBindingCache().path(fileName);
            return v320SoundQueryCache().path(ptr, fileName, path);
        };

        api["say"] = [luaManager = context.mLuaManager](
                         std::string_view fileName, const sol::object& object, sol::optional<std::string_view> text) {
            MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));
            MWBase::Environment::get().getSoundManager()->say(ptr, v317SoundBindingCache().path(fileName));
            if (text && Settings::gui().mSubtitles)
                luaManager->addUIMessage(*text);
        };
        api["stopSay"] = [](const sol::object& object) {
            MWWorld::Ptr ptr = getMutablePtrOrThrow(ObjectVariant(object));
            MWBase::Environment::get().getSoundManager()->stopSay(ptr);
        };
        api["isSayActive"] = [](const sol::object& object) {
            const MWWorld::Ptr& ptr = getPtrOrThrow(ObjectVariant(object));
            return MWBase::Environment::get().getSoundManager()->sayActive(ptr);
        };

        addRecordFunctionBinding<ESM::Sound>(api, context);

        // Sound record
        addUserType<ESM::Sound>(lua, "ESM3_Sound");

        return LuaUtil::makeReadOnly(api);
    }

    void addMutableSoundType(sol::state_view& lua)
    {
        addUserType<MutableRecord<ESM::Sound>>(lua, "ESM3_MutableSound");
    }

    ESM::Sound tableToSound(const sol::table& rec)
    {
        auto sound = Types::initFromTemplate<ESM::Sound>(rec);
        if (rec["volume"] != sol::nil)
            sound.mData.mVolume = rec["volume"];
        if (rec["minRange"] != sol::nil)
            sound.mData.mMinRange = rec["minRange"];
        if (rec["maxRange"] != sol::nil)
            sound.mData.mMaxRange = rec["maxRange"];
        if (rec["fileName"] != sol::nil)
            sound.mSound = Misc::ResourceHelpers::soundPathForESM3(rec["fileName"].get<std::string_view>());
        return sound;
    }
}
