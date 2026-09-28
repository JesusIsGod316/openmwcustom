#ifndef OPENMW_SOUND_WARMQUEUE_H
#define OPENMW_SOUND_WARMQUEUE_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace MWSound
{
    // Adapted from the upstream streamed/effect warming series. This version
    // bounds pending names and keeps in-flight deduplication. No world pointers
    // cross the boundary, no playback wait, and no OpenAL calls on this worker.
    class WarmQueue final
    {
    public:
        struct Item { std::string path; bool wholeFile=false; bool urgent=false; };
        struct Stats
        {
            std::uint64_t accepted=0, deduplicated=0, dropped=0, processed=0, skipped=0, failed=0;
            std::size_t pending=0, pathBytes=0, peakPending=0;
        };
        enum class Result { Accepted, Duplicate, Full, Stopping, Invalid };
        using Executor = std::function<bool(const Item&)>;
        WarmQueue(Executor execute, std::function<void()> threadInit = {},
            std::size_t maxItems = 4096, std::size_t maxPathBytes = 1024*1024);
        ~WarmQueue();
        WarmQueue(const WarmQueue&) = delete;
        WarmQueue& operator=(const WarmQueue&) = delete;
        Result enqueue(Item item);
        Stats stats() const;
    private:
        void run() noexcept;
        const Executor mExecute;
        const std::function<void()> mThreadInit;
        const std::size_t mMaxItems, mMaxPathBytes;
        mutable std::mutex mMutex;
        std::condition_variable mCondition;
        std::list<Item> mQueue;
        std::unordered_map<std::string, std::list<Item>::iterator> mQueued;
        std::optional<Item> mRunning;
        bool mUpgradeRunning = false;
        bool mStop = false;
        Stats mStats;
        std::thread mThread; // all state initialized before the worker starts
    };
}
#endif
