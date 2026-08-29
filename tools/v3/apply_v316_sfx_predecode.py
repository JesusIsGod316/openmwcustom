import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.16 SFX-predecode match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 SFX predecode patched {rel} ({count} match(es))")


def write_new(rel, content):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: expected new V3.16 SFX-predecode file")
    path.write_text(content, encoding="utf-8", newline="\n")
    print(f"V3.16 SFX predecode added {rel}")


CACHE_HPP = r'''#ifndef GAME_SOUND_SFXPREDECODECACHE_H
#define GAME_SOUND_SFXPREDECODECACHE_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <components/vfs/pathutil.hpp>

#include "sounddecoder.hpp"

namespace VFS
{
    class Manager;
}

namespace MWSound
{
    struct PredecodedSound
    {
        std::vector<char> mData;
        int mSampleRate = 0;
        ChannelConfig mChannels = ChannelConfig_Mono;
        SampleType mType = SampleType_UInt8;
    };

    // V3.16 aggressive path: decode likely buffered SFX on idle-priority worker
    // threads without touching OpenAL. The gameplay thread only consumes a ready
    // PCM blob and performs the small OpenAL buffer upload. A bounded decoded-data
    // reservoir prevents unbounded RAM use. A requested sound that is not ready
    // always falls back to the original synchronous path, preserving semantics.
    class SfxPredecodeCache
    {
    public:
        SfxPredecodeCache(const VFS::Manager& vfs, std::size_t maxBytes, unsigned workers);
        ~SfxPredecodeCache();

        SfxPredecodeCache(const SfxPredecodeCache&) = delete;
        SfxPredecodeCache& operator=(const SfxPredecodeCache&) = delete;

        void queue(std::vector<VFS::Path::Normalized>&& names);
        std::optional<PredecodedSound> take(VFS::Path::NormalizedView name);
        void discard(VFS::Path::NormalizedView name);

    private:
        void workerLoop();
        std::optional<PredecodedSound> decode(VFS::Path::NormalizedView name) const;

        const VFS::Manager& mVfs;
        const std::size_t mMaxBytes;
        std::mutex mMutex;
        std::condition_variable mCondition;
        std::deque<VFS::Path::Normalized> mQueue;
        std::unordered_set<VFS::Path::Normalized, VFS::Path::Hash, std::equal_to<>> mQueued;
        std::unordered_set<VFS::Path::Normalized, VFS::Path::Hash, std::equal_to<>> mCancelled;
        std::unordered_map<VFS::Path::Normalized, PredecodedSound, VFS::Path::Hash, std::equal_to<>> mReady;
        std::vector<std::thread> mWorkers;
        std::size_t mReadyBytes = 0;
        bool mStopping = false;
    };
}

#endif
'''

CACHE_CPP = r'''#include "sfxpredecodecache.hpp"

#include <memory>

#include <components/misc/resourcehelpers.hpp>
#include <components/misc/thread.hpp>
#include <components/vfs/manager.hpp>

#include "ffmpegdecoder.hpp"

namespace MWSound
{
    namespace
    {
        // Avoid turning long music-like assets accidentally stored as SFX into
        // giant speculative PCM allocations. Normal effects are far below this.
        constexpr std::size_t sMaxPredecodedEntryBytes = 16 * 1024 * 1024;
    }

    SfxPredecodeCache::SfxPredecodeCache(const VFS::Manager& vfs, std::size_t maxBytes, unsigned workers)
        : mVfs(vfs)
        , mMaxBytes(maxBytes)
    {
        if (mMaxBytes == 0)
            return;
        workers = std::clamp(workers, 1u, 2u);
        mWorkers.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
            mWorkers.emplace_back([this] { workerLoop(); });
    }

    SfxPredecodeCache::~SfxPredecodeCache()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mCondition.notify_all();
        for (std::thread& worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    void SfxPredecodeCache::queue(std::vector<VFS::Path::Normalized>&& names)
    {
        if (mWorkers.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (VFS::Path::Normalized& name : names)
            {
                if (name.empty() || mCancelled.contains(name) || mReady.contains(name) || mQueued.contains(name))
                    continue;
                mQueued.emplace(name);
                mQueue.emplace_back(std::move(name));
            }
        }
        mCondition.notify_all();
    }

    std::optional<PredecodedSound> SfxPredecodeCache::take(VFS::Path::NormalizedView name)
    {
        std::optional<PredecodedSound> result;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto it = mReady.find(name);
            if (it == mReady.end())
                return result;
            mReadyBytes -= it->second.mData.size();
            result.emplace(std::move(it->second));
            mReady.erase(it);
            mQueued.erase(name);
        }
        mCondition.notify_all();
        return result;
    }

    void SfxPredecodeCache::discard(VFS::Path::NormalizedView name)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mCancelled.emplace(name);
            if (const auto it = mReady.find(name); it != mReady.end())
            {
                mReadyBytes -= it->second.mData.size();
                mReady.erase(it);
            }
            mQueued.erase(name);
        }
        mCondition.notify_all();
    }

    std::optional<PredecodedSound> SfxPredecodeCache::decode(VFS::Path::NormalizedView name) const
    {
        try
        {
            DecoderPtr decoder = std::make_shared<FFmpegDecoder>(&mVfs, nullptr);
            decoder->open(Misc::ResourceHelpers::correctSoundPath(name, *decoder->mResourceMgr));

            PredecodedSound decoded;
            decoder->getInfo(&decoded.mSampleRate, &decoded.mChannels, &decoded.mType);
            decoder->readAll(decoded.mData);
            if (decoded.mData.empty() || decoded.mData.size() > sMaxPredecodedEntryBytes)
                return std::nullopt;
            return decoded;
        }
        catch (...)
        {
            // The original load path remains authoritative and will report any
            // real error if this sound is actually requested.
            return std::nullopt;
        }
    }

    void SfxPredecodeCache::workerLoop()
    {
        Misc::setCurrentThreadIdlePriority();

        for (;;)
        {
            VFS::Path::Normalized name;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCondition.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                if (mStopping)
                    return;
                name = std::move(mQueue.front());
                mQueue.pop_front();
                if (mCancelled.contains(name))
                {
                    mQueued.erase(name);
                    continue;
                }
            }

            std::optional<PredecodedSound> decoded = decode(name);
            if (!decoded)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueued.erase(name);
                continue;
            }

            const std::size_t bytes = decoded->mData.size();
            if (bytes > mMaxBytes)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueued.erase(name);
                continue;
            }

            std::unique_lock<std::mutex> lock(mMutex);
            mCondition.wait(lock, [this, &name, bytes] {
                return mStopping || mCancelled.contains(name) || mReadyBytes + bytes <= mMaxBytes;
            });
            if (mStopping)
                return;
            if (mCancelled.contains(name))
            {
                mQueued.erase(name);
                continue;
            }

            mReadyBytes += bytes;
            mReady.emplace(std::move(name), std::move(*decoded));
        }
    }
}
'''

write_new("apps/openmw/mwsound/sfxpredecodecache.hpp", CACHE_HPP)
write_new("apps/openmw/mwsound/sfxpredecodecache.cpp", CACHE_CPP)

replace_exact(
    "apps/openmw/CMakeLists.txt",
    "    soundmanagerimp openaloutput ffmpegdecoder headcache sound soundbuffer sounddecoder soundoutput\n",
    "    soundmanagerimp openaloutput ffmpegdecoder headcache sfxpredecodecache sound soundbuffer sounddecoder soundoutput\n",
)

replace_exact(
    "components/settings/categories/sound.hpp",
    '''        SettingValue<std::size_t> mHeadCacheSize{ mIndex, "Sound", "head cache size", makeClampSanitizerSize(0, 4095) };
        SettingValue<HrtfMode> mHrtfEnable''',
    '''        SettingValue<std::size_t> mHeadCacheSize{ mIndex, "Sound", "head cache size", makeClampSanitizerSize(0, 4095) };
        SettingValue<std::size_t> mSfxPredecodeCacheSize{
            mIndex, "Sound", "sfx predecode cache size", makeClampSanitizerSize(0, 4095) };
        SettingValue<int> mSfxPredecodeWorkers{
            mIndex, "Sound", "sfx predecode workers", makeClampSanitizerInt(0, 2) };
        SettingValue<HrtfMode> mHrtfEnable''',
)

replace_exact(
    "files/settings-default.cfg",
    '''head cache size = 0

# Specifies whether to enable HRTF processing.''',
    '''head cache size = 0

# V3.16 experimental buffered-SFX first-use predecode reservoir, in MB.
# Decoding happens on idle-priority workers; OpenAL upload remains on the
# gameplay thread. 0 disables the mechanism.
sfx predecode cache size = 0
sfx predecode workers = 0

# Specifies whether to enable HRTF processing.''',
)

replace_exact(
    "apps/openmw/mwsound/soundoutput.hpp",
    '''        virtual std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) = 0;
        virtual size_t unloadSound(Sound_Handle data) = 0;''',
    '''        virtual std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) = 0;
        virtual size_t unloadSound(Sound_Handle data) = 0;
        virtual void queueSoundPredecode(std::vector<VFS::Path::Normalized>&& names) = 0;''',
)

replace_exact(
    "apps/openmw/mwsound/openaloutput.hpp",
    '''    class Sound;
    class Stream;

    class OpenALOutput''',
    '''    class Sound;
    class Stream;
    class SfxPredecodeCache;

    class OpenALOutput''',
)

replace_exact(
    "apps/openmw/mwsound/openaloutput.hpp",
    '''        class DefaultDeviceThread;
        std::unique_ptr<DefaultDeviceThread> mDefaultDeviceThread;

        void initCommon2D''',
    '''        class DefaultDeviceThread;
        std::unique_ptr<DefaultDeviceThread> mDefaultDeviceThread;

        std::unique_ptr<SfxPredecodeCache> mSfxPredecodeCache;

        void initCommon2D''',
)

replace_exact(
    "apps/openmw/mwsound/openaloutput.hpp",
    '''        std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) override;
        size_t unloadSound(Sound_Handle data) override;''',
    '''        std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) override;
        size_t unloadSound(Sound_Handle data) override;
        void queueSoundPredecode(std::vector<VFS::Path::Normalized>&& names) override;''',
)

replace_exact(
    "apps/openmw/mwsound/openaloutput.cpp",
    '''#include "sounddecoder.hpp"
#include "soundmanagerimp.hpp"''',
    '''#include "sounddecoder.hpp"
#include "soundmanagerimp.hpp"
#include "sfxpredecodecache.hpp"''',
)

old_load = r'''    std::pair<Sound_Handle, size_t> OpenALOutput::loadSound(VFS::Path::NormalizedView fname)
    {
        getALError();

        std::vector<char> data;
        ALenum format = AL_NONE;
        int srate = 0;

        try
        {
            DecoderPtr decoder = mManager.getDecoder();
            decoder->open(Misc::ResourceHelpers::correctSoundPath(fname, *decoder->mResourceMgr));

            ChannelConfig chans;
            SampleType type;
            decoder->getInfo(&srate, &chans, &type);
            format = getALFormat(chans, type);
            if (format)
                decoder->readAll(data);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Failed to load audio from " << fname << ": " << e.what();
        }

        if (data.empty())'''
new_load = r'''    std::pair<Sound_Handle, size_t> OpenALOutput::loadSound(VFS::Path::NormalizedView fname)
    {
        getALError();

        std::vector<char> data;
        ALenum format = AL_NONE;
        int srate = 0;
        bool usedPredecoded = false;

        if (mSfxPredecodeCache)
        {
            if (std::optional<PredecodedSound> decoded = mSfxPredecodeCache->take(fname))
            {
                srate = decoded->mSampleRate;
                format = getALFormat(decoded->mChannels, decoded->mType);
                if (format != AL_NONE)
                {
                    data = std::move(decoded->mData);
                    usedPredecoded = true;
                }
            }
        }

        if (!usedPredecoded)
        {
            if (mSfxPredecodeCache)
                mSfxPredecodeCache->discard(fname);
            try
            {
                DecoderPtr decoder = mManager.getDecoder();
                decoder->open(Misc::ResourceHelpers::correctSoundPath(fname, *decoder->mResourceMgr));

                ChannelConfig chans;
                SampleType type;
                decoder->getInfo(&srate, &chans, &type);
                format = getALFormat(chans, type);
                if (format)
                    decoder->readAll(data);
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to load audio from " << fname << ": " << e.what();
            }
        }

        if (data.empty())'''
replace_exact("apps/openmw/mwsound/openaloutput.cpp", old_load, new_load)

replace_exact(
    "apps/openmw/mwsound/openaloutput.cpp",
    '''    size_t OpenALOutput::unloadSound(Sound_Handle data)
    {''',
    '''    void OpenALOutput::queueSoundPredecode(std::vector<VFS::Path::Normalized>&& names)
    {
        if (mSfxPredecodeCache)
            mSfxPredecodeCache->queue(std::move(names));
    }

    size_t OpenALOutput::unloadSound(Sound_Handle data)
    {''',
)

replace_exact(
    "apps/openmw/mwsound/openaloutput.cpp",
    '''        , mDefaultEffect(0)
        , mEffectSlot(0)
        , mStreamThread(std::make_unique<StreamThread>())
    {
    }''',
    '''        , mDefaultEffect(0)
        , mEffectSlot(0)
        , mStreamThread(std::make_unique<StreamThread>())
    {
        const std::size_t predecodeMb = Settings::sound().mSfxPredecodeCacheSize;
        const unsigned predecodeWorkers = static_cast<unsigned>(Settings::sound().mSfxPredecodeWorkers);
        if (predecodeMb != 0 && predecodeWorkers != 0)
            mSfxPredecodeCache = std::make_unique<SfxPredecodeCache>(
                *mgr.mVFS, predecodeMb * 1024 * 1024, predecodeWorkers);
    }''',
)

replace_exact(
    "apps/openmw/mwsound/soundbuffer.hpp",
    '''#include <deque>
#include <unordered_map>''',
    '''#include <deque>
#include <unordered_map>
#include <vector>''',
)

replace_exact(
    "apps/openmw/mwsound/soundbuffer.hpp",
    '''        // Lookup for a sound by file name, and ensure it's ready for use.
        SoundBuffer* load(VFS::Path::NormalizedView fileName);

        void use''',
    '''        // Lookup for a sound by file name, and ensure it's ready for use.
        SoundBuffer* load(VFS::Path::NormalizedView fileName);

        // Prepare ESM sound metadata on the main thread and return unique resource
        // names suitable for background PCM predecode.
        std::vector<VFS::Path::Normalized> getResourceNamesForPredecode();

        void use''',
)

replace_exact(
    "apps/openmw/mwsound/soundbuffer.hpp",
    '''        std::size_t mBufferCacheSize = 0;
        // NOTE: unused buffers are stored in front-newest order.''',
    '''        std::size_t mBufferCacheSize = 0;
        bool mSoundRecordsPrepared = false;
        // NOTE: unused buffers are stored in front-newest order.''',
)

replace_exact(
    "apps/openmw/mwsound/soundbuffer.hpp",
    '''        SoundBuffer* loadSfx(SoundBuffer* sfx);

        SoundOutput* mOutput;''',
    '''        SoundBuffer* loadSfx(SoundBuffer* sfx);
        void prepareSoundRecords();

        SoundOutput* mOutput;''',
)

replace_exact(
    "apps/openmw/mwsound/soundbuffer.cpp",
    '''#include <algorithm>
#include <cmath>''',
    '''#include <algorithm>
#include <cmath>
#include <unordered_set>''',
)

old_records = r'''    SoundBuffer* SoundBufferPool::load(const ESM::RefId& soundId)
    {
        if (mBufferNameMap.empty())
        {
            const MWWorld::ESMStore* esmstore = MWBase::Environment::get().getESMStore();
            for (const ESM::Sound& sound : esmstore->get<ESM::Sound>())
                insertSound(sound.mId, sound);
            for (const ESM4::Sound& sound : esmstore->get<ESM4::Sound>())
                insertSound(sound.mId, sound);
            for (const ESM4::SoundReference& sound : esmstore->get<ESM4::SoundReference>())
                insertSound(sound.mId, sound);
        }

        SoundBuffer* sfx;'''
new_records = r'''    void SoundBufferPool::prepareSoundRecords()
    {
        if (mSoundRecordsPrepared)
            return;
        const MWWorld::ESMStore* esmstore = MWBase::Environment::get().getESMStore();
        for (const ESM::Sound& sound : esmstore->get<ESM::Sound>())
            insertSound(sound.mId, sound);
        for (const ESM4::Sound& sound : esmstore->get<ESM4::Sound>())
            insertSound(sound.mId, sound);
        for (const ESM4::SoundReference& sound : esmstore->get<ESM4::SoundReference>())
            insertSound(sound.mId, sound);
        mSoundRecordsPrepared = true;
    }

    std::vector<VFS::Path::Normalized> SoundBufferPool::getResourceNamesForPredecode()
    {
        prepareSoundRecords();
        std::vector<VFS::Path::Normalized> result;
        result.reserve(mSoundBuffers.size());
        std::unordered_set<VFS::Path::Normalized, VFS::Path::Hash, std::equal_to<>> seen;
        for (const SoundBuffer& sfx : mSoundBuffers)
        {
            if (seen.emplace(sfx.getResourceName()).second)
                result.emplace_back(sfx.getResourceName());
        }
        return result;
    }

    SoundBuffer* SoundBufferPool::load(const ESM::RefId& soundId)
    {
        prepareSoundRecords();

        SoundBuffer* sfx;'''
replace_exact("apps/openmw/mwsound/soundbuffer.cpp", old_records, new_records)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''        Sound* mCurrentRegionSound;

        SoundBuffer* insertSound''',
    '''        Sound* mCurrentRegionSound;
        bool mV316SfxPrewarmQueued = false;

        SoundBuffer* insertSound''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''        if (isMainMenu && !isMusicPlaying())
        {
            if (mVFS->exists(MWSound::titleMusic))
                streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        }

        updateSounds(duration);''',
    '''        if (isMainMenu && !isMusicPlaying())
        {
            if (mVFS->exists(MWSound::titleMusic))
                streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        }

        if (state != MWBase::StateManager::State_NoGame && !mV316SfxPrewarmQueued
            && Settings::sound().mSfxPredecodeCacheSize != 0 && Settings::sound().mSfxPredecodeWorkers != 0)
        {
            mOutput->queueSoundPredecode(mSoundBuffers.getResourceNamesForPredecode());
            mV316SfxPrewarmQueued = true;
        }

        updateSounds(duration);''',
)

# Launcher: only Mode89 enables first-use background predecode. Mode88 remains a
# lower-risk high-retention candidate so attribution is preserved.
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old_defaults = "$V316BufferCacheMax = ''\n$RendererProfiling"
new_defaults = "$V316BufferCacheMax = ''\n$V316SfxPredecodeCacheSize = '0'\n$V316SfxPredecodeWorkers = '0'\n$RendererProfiling"
if text.count(old_defaults) != 1:
    raise RuntimeError("V3.16 SFX predecode launcher default anchor mismatch")
text = text.replace(old_defaults, new_defaults, 1)

lines = text.splitlines()
matches = [i for i, line in enumerate(lines) if line.startswith("        '89' {")]
if len(matches) != 1:
    raise RuntimeError(f"V3.16 SFX predecode expected one Mode89 line, found {len(matches)}")
i = matches[0]
line = lines[i]
if not line.rstrip().endswith("}"):
    raise RuntimeError("V3.16 Mode89 launcher line has unexpected layout")
line = line.rstrip()[:-1].rstrip()
line += "; $V316SfxPredecodeCacheSize = '384'; $V316SfxPredecodeWorkers = '1' }"
lines[i] = line
text = "\n".join(lines) + "\n"

setting_anchor = "        Set-IniValue $SettingsPath 'Sound' 'head cache size' $V316HeadCacheSize"
if text.count(setting_anchor) != 1:
    raise RuntimeError("V3.16 SFX predecode launcher setting anchor mismatch")
text = text.replace(
    setting_anchor,
    setting_anchor
    + "\n        Set-IniValue $SettingsPath 'Sound' 'sfx predecode cache size' $V316SfxPredecodeCacheSize"
    + "\n        Set-IniValue $SettingsPath 'Sound' 'sfx predecode workers' $V316SfxPredecodeWorkers",
    1,
)

text = text.replace(
    "Write-Host ' 89 = V3.16 aggressive hitch: audio128 + 512/768MB decoded SFX retention'",
    "Write-Host ' 89 = V3.16 aggressive: audio128 + SFX retention + 384MB idle predecode'",
    1,
)
launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.16-HITCH-LAYER.txt"
with marker.open("a", encoding="utf-8", newline="\n") as f:
    f.write("mode89_sfx_predecode_cache_mb=384\n")
    f.write("mode89_sfx_predecode_workers=1_idle_priority\n")
    f.write("sfx_predecode_openal_calls_on_worker=0\n")
    f.write("sfx_predecode_sync_fallback=original_semantics\n")

print("V3.16 aggressive background buffered-SFX predecode path applied")
