#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_TERRAINWORLDSERVICE_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_TERRAINWORLDSERVICE_H

#include <components/rendercore/terrainchunkproducer.hpp>
#include <components/rendercore/terrainpreparationservice.hpp>
#include <components/rendercore/terrainresidencyplanner.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RenderNative
{
    enum class TerrainWorldSyncStatus : std::uint8_t
    {
        Applied,
        PartiallyApplied,
        AlreadyCurrent,
        InvalidSource,
        PreparationFailed,
        PublishFailed,
    };

    struct TerrainWorldSyncResult
    {
        TerrainWorldSyncStatus status = TerrainWorldSyncStatus::InvalidSource;
        RenderCore::TerrainChunkPublishStatus publishStatus = RenderCore::TerrainChunkPublishStatus::AlreadyPresent;
        RenderCore::TerrainPreparationRequestStatus requestStatus
            = RenderCore::TerrainPreparationRequestStatus::Unchanged;
        std::string diagnostic;

        [[nodiscard]] bool accepted() const noexcept
        {
            return status == TerrainWorldSyncStatus::Applied
                || status == TerrainWorldSyncStatus::PartiallyApplied
                || status == TerrainWorldSyncStatus::AlreadyCurrent;
        }
    };

    // Phase 2 neutral terrain ownership. The caller supplies authoritative LAND
    // extraction as a builder; this service owns residency, background
    // preparation, bounded publication and RenderWorld terrain lifetime.
    class TerrainWorldService final
    {
    public:
        using Builder = RenderCore::TerrainPreparationService::Builder;

        TerrainWorldService(RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
            : mWorld(world)
            , mTerrain(world, publisher)
            , mObservedEpoch(world.epoch())
        {
        }

        [[nodiscard]] std::vector<RenderCore::TerrainResidencyCell> updateResidency(
            std::string_view worldspaceIdentity, std::int32_t gridX, std::int32_t gridY)
        {
            synchronizeEpoch();
            return mResidency.update(worldspaceIdentity, gridX, gridY);
        }

        [[nodiscard]] TerrainWorldSyncResult clear()
        {
            synchronizeEpoch();
            mResidency.reset();
            mPendingPublication.clear();
            if (mPreparation)
            {
                static_cast<void>(
                    mPreparation->request(std::span<const RenderCore::TerrainPreparationRequest>{}));
                static_cast<void>(mPreparation->takeReady());
            }
            const RenderCore::TerrainChunkPublishStatus published = mTerrain.synchronize(std::nullopt);
            if (published == RenderCore::TerrainChunkPublishStatus::Applied)
                return { TerrainWorldSyncStatus::Applied, published };
            if (published == RenderCore::TerrainChunkPublishStatus::AlreadyPresent)
                return { TerrainWorldSyncStatus::AlreadyCurrent, published };
            return { TerrainWorldSyncStatus::PublishFailed, published,
                RenderCore::TerrainPreparationRequestStatus::Unchanged,
                "native terrain clear failed with publish status "
                    + std::to_string(static_cast<unsigned int>(published)) };
        }

        [[nodiscard]] TerrainWorldSyncResult synchronize(std::span<const RenderCore::TerrainPreparationRequest> desired,
            Builder builder, RenderCore::TerrainPublicationLimits limits = { 4, 8u * 1024u * 1024u })
        {
            synchronizeEpoch();
            if (desired.empty() || !builder)
                return { TerrainWorldSyncStatus::InvalidSource };

            const auto required = std::ranges::find_if(desired, &RenderCore::TerrainPreparationRequest::required);
            if (required == desired.end())
            {
                return { TerrainWorldSyncStatus::InvalidSource, RenderCore::TerrainChunkPublishStatus::AlreadyPresent,
                    RenderCore::TerrainPreparationRequestStatus::Invalid,
                    "native terrain desired set contains no required current chunk" };
            }

            bool changed = false;
            RenderCore::TerrainChunkPublishStatus publishStatus = RenderCore::TerrainChunkPublishStatus::AlreadyPresent;
            if (!mTerrain.contains(required->identity))
            {
                std::optional<RenderCore::TerrainChunkSource> source = builder(*required, std::stop_token{});
                if (!source || !RenderCore::validTerrainChunkSource(*source) || source->identity != required->identity)
                {
                    return { TerrainWorldSyncStatus::InvalidSource, publishStatus,
                        RenderCore::TerrainPreparationRequestStatus::Invalid,
                        "authoritative LAND builder could not produce the required native terrain chunk" };
                }

                publishStatus = mTerrain.synchronize(source);
                if (publishStatus != RenderCore::TerrainChunkPublishStatus::Applied
                    && publishStatus != RenderCore::TerrainChunkPublishStatus::AlreadyPresent)
                {
                    return { TerrainWorldSyncStatus::PublishFailed, publishStatus,
                        RenderCore::TerrainPreparationRequestStatus::Unchanged,
                        "required native terrain chunk publication failed" };
                }
                changed = publishStatus == RenderCore::TerrainChunkPublishStatus::Applied;
            }

            if (!mPreparation)
                mPreparation = std::make_unique<RenderCore::TerrainPreparationService>(std::move(builder));

            const RenderCore::TerrainPreparationRequestStatus requested = mPreparation->request(desired);
            if (requested == RenderCore::TerrainPreparationRequestStatus::Invalid
                || requested == RenderCore::TerrainPreparationRequestStatus::GenerationExhausted)
            {
                return { TerrainWorldSyncStatus::PreparationFailed, publishStatus, requested,
                    "native terrain preparation rejected the desired resident set" };
            }
            if (requested == RenderCore::TerrainPreparationRequestStatus::Accepted)
                mPendingPublication.clear();

            if (std::optional<RenderCore::PreparedTerrainSet> ready = mPreparation->takeReady())
            {
                if (!ready->requiredChunksReady)
                {
                    return { TerrainWorldSyncStatus::PreparationFailed, publishStatus, requested,
                        "native terrain preparation failed for a required resident chunk" };
                }
                mPendingPublication = std::move(ready->chunks);
            }

            if (!mPendingPublication.empty())
            {
                publishStatus = mTerrain.synchronize(
                    std::span<const RenderCore::TerrainChunkSource>(mPendingPublication), limits);
                if (publishStatus != RenderCore::TerrainChunkPublishStatus::Applied
                    && publishStatus != RenderCore::TerrainChunkPublishStatus::PartiallyApplied
                    && publishStatus != RenderCore::TerrainChunkPublishStatus::AlreadyPresent)
                {
                    return { TerrainWorldSyncStatus::PublishFailed, publishStatus, requested,
                        "prepared native terrain set publication failed" };
                }
                if (publishStatus != RenderCore::TerrainChunkPublishStatus::PartiallyApplied)
                    mPendingPublication.clear();

                if (publishStatus == RenderCore::TerrainChunkPublishStatus::PartiallyApplied)
                    return { TerrainWorldSyncStatus::PartiallyApplied, publishStatus, requested };
                if (publishStatus == RenderCore::TerrainChunkPublishStatus::Applied)
                    changed = true;
            }

            return { changed ? TerrainWorldSyncStatus::Applied : TerrainWorldSyncStatus::AlreadyCurrent,
                publishStatus, requested };
        }

        void stopBackgroundPreparation() { mPreparation.reset(); }

    private:
        void synchronizeEpoch()
        {
            if (mObservedEpoch == mWorld.epoch())
                return;
            mPreparation.reset();
            mPendingPublication.clear();
            mResidency.reset();
            mObservedEpoch = mWorld.epoch();
        }

        RenderCore::RenderWorld& mWorld;
        RenderCore::TerrainChunkProducer mTerrain;
        std::unique_ptr<RenderCore::TerrainPreparationService> mPreparation;
        RenderCore::TerrainResidencyPlanner mResidency;
        std::vector<RenderCore::TerrainChunkSource> mPendingPublication;
        RenderCore::WorldEpoch mObservedEpoch;
    };
}

#endif
