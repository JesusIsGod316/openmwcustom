#include "sfxpredecodecache.hpp"

#include <algorithm>
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
                if (name.value().empty() || mCancelled.contains(name) || mReady.contains(name) || mQueued.contains(name))
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
            if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
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
            if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
        }
        mCondition.notify_all();
    }

    std::optional<PredecodedSound> SfxPredecodeCache::decode(VFS::Path::NormalizedView name) const
    {
        try
        {
            std::unique_ptr<SoundDecoder> decoder = std::make_unique<FFmpegDecoder>(&mVfs, nullptr);
            decoder->open(Misc::ResourceHelpers::correctSoundPath(name, mVfs));

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
                    if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
                    continue;
                }
            }

            std::optional<PredecodedSound> decoded = decode(name);
            if (!decoded)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
                continue;
            }

            const std::size_t bytes = decoded->mData.size();
            if (bytes > mMaxBytes)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
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
                if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())
                    mQueued.erase(queuedIt);
                continue;
            }

            mReadyBytes += bytes;
            mReady.emplace(std::move(name), std::move(*decoded));
        }
    }
}
