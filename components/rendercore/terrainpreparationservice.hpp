#ifndef OPENMW_COMPONENTS_RENDERCORE_TERRAINPREPARATIONSERVICE_H
#define OPENMW_COMPONENTS_RENDERCORE_TERRAINPREPARATIONSERVICE_H

#include "terrainchunkproducer.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RenderCore
{
    struct TerrainPreparationRequest
    {
        std::string identity;
        std::string worldspaceIdentity;
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
        std::uint32_t lodLevel = 0;
        std::uint8_t stitchMask = 0;
        std::uint64_t contentRevision = 0;
        bool required = false;

        friend bool operator==(const TerrainPreparationRequest&, const TerrainPreparationRequest&) = default;
    };

    enum class TerrainPreparationRequestStatus : std::uint8_t
    {
        Accepted,
        Unchanged,
        Invalid,
        GenerationExhausted,
    };

    struct PreparedTerrainSet
    {
        std::uint64_t generation = 0;
        std::vector<TerrainChunkSource> chunks;
        std::vector<std::string> failedIdentities;
        std::uint64_t preparedBytes = 0;
        bool requiredChunksReady = false;
        bool budgetLimited = false;
    };

    struct TerrainPreparationLimits
    {
        std::size_t maxChunks = 32;
        std::uint64_t maxPreparedBytes = 64u * 1024u * 1024u;
    };

    // Owns one coarse terrain-preparation lane. Requests replace, rather than
    // append to, pending work; stale generations are abandoned between chunks.
    // The frame thread only submits desired state and polls completed immutable
    // sets, so it never queues work and waits for that work in the same frame.
    class TerrainPreparationService final
    {
    public:
        using Builder
            = std::function<std::optional<TerrainChunkSource>(const TerrainPreparationRequest&, std::stop_token)>;

        explicit TerrainPreparationService(Builder builder, TerrainPreparationLimits limits = {})
            : mBuilder(std::move(builder))
            , mLimits(limits)
            , mWorker([this](std::stop_token stop) { run(stop); })
        {
        }

        ~TerrainPreparationService()
        {
            mWorker.request_stop();
            mCondition.notify_all();
        }

        TerrainPreparationService(const TerrainPreparationService&) = delete;
        TerrainPreparationService& operator=(const TerrainPreparationService&) = delete;

        [[nodiscard]] TerrainPreparationRequestStatus request(std::span<const TerrainPreparationRequest> requested)
        {
            std::vector<TerrainPreparationRequest> canonical(requested.begin(), requested.end());
            std::ranges::sort(canonical, {}, &TerrainPreparationRequest::identity);
            if (!valid(canonical) || canonical.size() > mLimits.maxChunks)
                return TerrainPreparationRequestStatus::Invalid;

            std::scoped_lock lock(mMutex);
            if (canonical == mDesired)
                return TerrainPreparationRequestStatus::Unchanged;
            if (mNextGeneration == std::numeric_limits<std::uint64_t>::max())
                return TerrainPreparationRequestStatus::GenerationExhausted;

            mDesired = canonical;
            mReady.reset();
            mPending = Work{ ++mNextGeneration, std::move(canonical) };
            mCondition.notify_one();
            return TerrainPreparationRequestStatus::Accepted;
        }

        [[nodiscard]] std::optional<PreparedTerrainSet> takeReady()
        {
            std::scoped_lock lock(mMutex);
            return std::exchange(mReady, std::nullopt);
        }

        [[nodiscard]] std::uint64_t latestRequestedGeneration() const noexcept
        {
            std::scoped_lock lock(mMutex);
            return mNextGeneration;
        }

    private:
        struct Work
        {
            std::uint64_t generation = 0;
            std::vector<TerrainPreparationRequest> requests;
        };

        struct CachedChunk
        {
            TerrainPreparationRequest request;
            TerrainChunkSource source;
        };

        [[nodiscard]] static bool valid(const std::vector<TerrainPreparationRequest>& requests) noexcept
        {
            for (std::size_t i = 0; i < requests.size(); ++i)
            {
                const TerrainPreparationRequest& request = requests[i];
                if (request.identity.empty() || request.worldspaceIdentity.empty())
                    return false;
                if (i != 0 && request.worldspaceIdentity != requests.front().worldspaceIdentity)
                    return false;
                if (i != 0 && request.identity == requests[i - 1].identity)
                    return false;
                for (std::size_t j = i + 1; j < requests.size(); ++j)
                {
                    if (request.gridX == requests[j].gridX && request.gridY == requests[j].gridY
                        && request.lodLevel == requests[j].lodLevel)
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool stale(std::uint64_t generation) const
        {
            std::scoped_lock lock(mMutex);
            return mPending && mPending->generation > generation;
        }

        [[nodiscard]] static bool matches(
            const TerrainChunkSource& source, const TerrainPreparationRequest& request) noexcept
        {
            return validTerrainChunkSource(source) && source.identity == request.identity
                && source.worldspaceIdentity == request.worldspaceIdentity && source.gridX == request.gridX
                && source.gridY == request.gridY && source.lodLevel == request.lodLevel
                && source.stitchMask == request.stitchMask;
        }

        void run(std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                Work work;
                std::map<std::string, CachedChunk, std::less<>> cache;
                {
                    std::unique_lock lock(mMutex);
                    mCondition.wait(lock, stop, [this] { return mPending.has_value(); });
                    if (stop.stop_requested())
                        return;
                    work = std::move(*mPending);
                    mPending.reset();
                    cache = mCache;
                }

                PreparedTerrainSet result;
                result.generation = work.generation;
                std::map<std::string, CachedChunk, std::less<>> nextCache;
                bool abandoned = false;
                bool requiredReady = true;
                for (const TerrainPreparationRequest& request : work.requests)
                {
                    if (stop.stop_requested() || stale(work.generation))
                    {
                        abandoned = true;
                        break;
                    }

                    std::optional<TerrainChunkSource> source;
                    const auto cached = cache.find(request.identity);
                    if (cached != cache.end() && cached->second.request == request)
                        source = cached->second.source;
                    else
                    {
                        try
                        {
                            source = mBuilder ? mBuilder(request, stop) : std::nullopt;
                        }
                        catch (...)
                        {
                            source.reset();
                        }
                    }

                    if (!source || !matches(*source, request))
                    {
                        result.failedIdentities.push_back(request.identity);
                        requiredReady = requiredReady && !request.required;
                        continue;
                    }
                    const std::optional<std::uint64_t> bytes = terrainMeshPayloadBytes(*source->mesh);
                    if (!bytes || *bytes > mLimits.maxPreparedBytes - result.preparedBytes)
                    {
                        result.failedIdentities.push_back(request.identity);
                        result.budgetLimited = true;
                        requiredReady = requiredReady && !request.required;
                        continue;
                    }
                    result.preparedBytes += *bytes;
                    nextCache.emplace(request.identity, CachedChunk{ request, *source });
                    result.chunks.push_back(std::move(*source));
                }
                if (abandoned)
                    continue;

                result.requiredChunksReady = requiredReady;
                std::scoped_lock lock(mMutex);
                if (mPending && mPending->generation > work.generation)
                    continue;
                mCache = std::move(nextCache);
                mReady = std::move(result);
            }
        }

        Builder mBuilder;
        TerrainPreparationLimits mLimits;
        mutable std::mutex mMutex;
        std::condition_variable_any mCondition;
        std::uint64_t mNextGeneration = 0;
        std::vector<TerrainPreparationRequest> mDesired;
        std::optional<Work> mPending;
        std::optional<PreparedTerrainSet> mReady;
        std::map<std::string, CachedChunk, std::less<>> mCache;
        std::jthread mWorker;
    };
}

#endif
