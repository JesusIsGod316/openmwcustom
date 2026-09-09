#ifndef OPENMW_COMPONENTS_NIFRENDER_STATICMODELCACHE_H
#define OPENMW_COMPONENTS_NIFRENDER_STATICMODELCACHE_H

#include "translationpublish.hpp"

#include <components/rendercore/renderworld.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace NifRender
{
    enum class StaticModelCacheStatus : std::uint8_t
    {
        Published,
        Reused,
        InvalidBundle,
        ContentConflict,
        PublishFailed,
    };

    struct StaticModelCacheResult
    {
        StaticModelCacheStatus status = StaticModelCacheStatus::InvalidBundle;
        TranslationPublishStatus publishStatus = TranslationPublishStatus::InvalidBundle;
        RenderCore::ModelHandle model;
        std::optional<RenderCore::SkeletonHandle> skeleton;

        [[nodiscard]] bool available() const noexcept
        {
            return status == StaticModelCacheStatus::Published || status == StaticModelCacheStatus::Reused;
        }
    };

    // Session-local cache for the winning VFS model translation. A normalized
    // source identity maps to exactly one content identity for a world epoch.
    // If mod/content configuration changes underneath the live world, fail
    // closed instead of silently mixing resources; hot reload needs a separate
    // atomic replacement contract. World reset discards every stale binding.
    class StaticModelCache final
    {
    public:
        StaticModelCache(RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
            : mWorld(world)
            , mPublisher(publisher)
            , mObservedEpoch(world.epoch())
        {
        }

        [[nodiscard]] std::optional<RenderCore::ModelHandle> find(std::string_view sourceIdentity)
        {
            synchronizeEpoch();
            const auto found = mEntries.find(sourceIdentity);
            if (found == mEntries.end())
                return std::nullopt;
            if (!mWorld.get(found->second.binding.model)
                || (found->second.skeleton && !mWorld.get(*found->second.skeleton)))
            {
                mEntries.erase(found);
                return std::nullopt;
            }
            return found->second.binding.model;
        }

        [[nodiscard]] std::optional<RenderCore::SkeletonHandle> findSkeleton(std::string_view sourceIdentity)
        {
            synchronizeEpoch();
            const auto found = mEntries.find(sourceIdentity);
            if (found == mEntries.end())
                return std::nullopt;
            if (!mWorld.get(found->second.binding.model)
                || (found->second.skeleton && !mWorld.get(*found->second.skeleton)))
            {
                mEntries.erase(found);
                return std::nullopt;
            }
            return found->second.skeleton;
        }

        [[nodiscard]] StaticModelCacheResult publish(const TranslationBundle& bundle)
        {
            synchronizeEpoch();
            if (!bundle.valid() || bundle.hasErrors() || bundle.sourceIdentity.empty() || bundle.contentIdentity.empty()
                || bundle.model.sourceIdentity != bundle.sourceIdentity
                || bundle.model.contentIdentity != bundle.contentIdentity)
                return {};

            const auto existing = mEntries.find(bundle.sourceIdentity);
            if (existing != mEntries.end())
            {
                if (!mWorld.get(existing->second.binding.model))
                {
                    mEntries.erase(existing);
                }
                else
                {
                    if (existing->second.contentIdentity != bundle.contentIdentity)
                    {
                        return { StaticModelCacheStatus::ContentConflict, TranslationPublishStatus::InvalidBundle,
                            existing->second.binding.model, existing->second.skeleton };
                    }
                    return { StaticModelCacheStatus::Reused, TranslationPublishStatus::Applied,
                        existing->second.binding.model, existing->second.skeleton };
                }
            }

            TranslationPublishResult published = publishTranslation(mWorld, mPublisher, bundle);
            if (!published.applied())
                return { StaticModelCacheStatus::PublishFailed, published.status, {}, {} };

            Entry entry;
            entry.contentIdentity = bundle.contentIdentity;
            entry.binding = std::move(published.binding);
            if (bundle.model.skeleton)
                entry.skeleton = entry.binding.skeletons[bundle.model.skeleton->value()];
            const RenderCore::ModelHandle model = entry.binding.model;
            const std::optional<RenderCore::SkeletonHandle> skeleton = entry.skeleton;
            mEntries.emplace(bundle.sourceIdentity, std::move(entry));
            return { StaticModelCacheStatus::Published, TranslationPublishStatus::Applied, model, skeleton };
        }

        [[nodiscard]] std::size_t size() const noexcept { return mEntries.size(); }

    private:
        struct Entry
        {
            std::string contentIdentity;
            TranslationBinding binding;
            std::optional<RenderCore::SkeletonHandle> skeleton;
        };

        void synchronizeEpoch()
        {
            if (mObservedEpoch == mWorld.epoch())
                return;
            mObservedEpoch = mWorld.epoch();
            mEntries.clear();
        }

        RenderCore::RenderWorld& mWorld;
        RenderCore::RenderWorldPublisher& mPublisher;
        RenderCore::WorldEpoch mObservedEpoch;
        std::map<std::string, Entry, std::less<>> mEntries;
    };
}

#endif
