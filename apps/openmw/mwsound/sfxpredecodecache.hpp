#ifndef GAME_SOUND_SFXPREDECODECACHE_H
#define GAME_SOUND_SFXPREDECODECACHE_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
