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

            // OpenMW body/equipment parts are allowed to carry valid RigGeometry
            // whose NiSkinInstance bones live outside the NIF's rendered root.
            // SceneUtil::attach deliberately copies only that rig into the actor's
            // already-authoritative skeleton and resolves the skin by bone name.
            // Static translation used to reject those assets while trying to
            // synthesize a standalone skeleton, which made otherwise valid NPC
            // parts impossible to publish through the neutral cache.
            //
            // Keep ordinary translation fail-closed. We only downgrade errors
            // that are exclusively about constructing a source-local skeleton
            // from an otherwise-valid skinned model. The SkinPayload itself must
            // still pass the neutral contract, and actor composition subsequently
            // validates every referenced bone against the authoritative actor
            // skeleton before the model can be used.
            TranslationBundle externallySkinnedBundle;
            const TranslationBundle* publishable = &bundle;
            if (bundle.hasErrors())
            {
                externallySkinnedBundle = bundle;
                if (!acceptExternalSkeletonOnlyDiagnostics(externallySkinnedBundle))
                    return {};
                publishable = &externallySkinnedBundle;
            }

            if (!publishable->valid() || publishable->hasErrors() || publishable->sourceIdentity.empty()
                || publishable->contentIdentity.empty() || publishable->model.sourceIdentity != publishable->sourceIdentity
                || publishable->model.contentIdentity != publishable->contentIdentity)
                return {};

            const auto existing = mEntries.find(publishable->sourceIdentity);
            if (existing != mEntries.end())
            {
                if (!mWorld.get(existing->second.binding.model))
                {
                    mEntries.erase(existing);
                }
                else
                {
                    if (existing->second.contentIdentity != publishable->contentIdentity)
                    {
                        return { StaticModelCacheStatus::ContentConflict, TranslationPublishStatus::InvalidBundle,
                            existing->second.binding.model, existing->second.skeleton };
                    }
                    return { StaticModelCacheStatus::Reused, TranslationPublishStatus::Applied,
                        existing->second.binding.model, existing->second.skeleton };
                }
            }

            TranslationPublishResult published = publishTranslation(mWorld, mPublisher, *publishable);
            if (!published.applied())
                return { StaticModelCacheStatus::PublishFailed, published.status, {}, {} };

            Entry entry;
            entry.contentIdentity = publishable->contentIdentity;
            entry.binding = std::move(published.binding);
            if (publishable->model.skeleton)
                entry.skeleton = entry.binding.skeletons[publishable->model.skeleton->value()];
            const RenderCore::ModelHandle model = entry.binding.model;
            const std::optional<RenderCore::SkeletonHandle> skeleton = entry.skeleton;
            mEntries.emplace(publishable->sourceIdentity, std::move(entry));
            return { StaticModelCacheStatus::Published, TranslationPublishStatus::Applied, model, skeleton };
        }

        [[nodiscard]] std::size_t size() const noexcept { return mEntries.size(); }

    private:
        [[nodiscard]] static bool acceptExternalSkeletonOnlyDiagnostics(TranslationBundle& bundle)
        {
            if (bundle.model.skeleton)
                return false;

            bool hasSkinnedMesh = false;
            bool everySkinnedMeshNamesRoot = true;
            for (const TranslatedMesh& mesh : bundle.meshes)
            {
                if (!mesh.record.skin)
                    continue;
                hasSkinnedMesh = true;
                everySkinnedMeshNamesRoot = everySkinnedMeshNamesRoot && !mesh.record.skin->rootBoneName.empty();
            }
            if (!hasSkinnedMesh)
                return false;

            bool sawRelaxableError = false;
            for (const TranslationDiagnostic& diagnostic : bundle.diagnostics)
            {
                if (diagnostic.severity != DiagnosticSeverity::Error)
                    continue;

                if (diagnostic.code == "skin.unresolved_skeleton_space")
                {
                    // The other diagnostic sharing this code means the geometry
                    // itself is unreachable and remains fatal. Only an external
                    // root-bone reference is valid for actor-part composition.
                    if (!everySkinnedMeshNamesRoot
                        || diagnostic.message
                            != "Skinned geometry names a root bone outside the translated model hierarchy")
                        return false;
                    sawRelaxableError = true;
                    continue;
                }

                if (diagnostic.code == "skin.non_invertible_root_space")
                {
                    // A rooted actor part is rebound to the master actor skeleton,
                    // so the donor root transform is not authoritative. Keep the
                    // rootless fallback strict because it really does depend on
                    // the local parent transform chain.
                    if (!everySkinnedMeshNamesRoot)
                        return false;
                    sawRelaxableError = true;
                    continue;
                }

                if (diagnostic.code == "skeleton.bone_outside_model"
                    || diagnostic.code == "skeleton.shared_bone_node"
                    || diagnostic.code == "skeleton.non_invertible_bind"
                    || diagnostic.code == "skeleton.invalid_payload")
                {
                    sawRelaxableError = true;
                    continue;
                }

                return false;
            }

            if (!sawRelaxableError)
                return false;

            for (TranslationDiagnostic& diagnostic : bundle.diagnostics)
            {
                if (diagnostic.severity != DiagnosticSeverity::Error)
                    continue;
                diagnostic.severity = DiagnosticSeverity::Info;
                diagnostic.message += " (published as an externally skinned model; instance composition supplies the authoritative skeleton)";
            }
            return true;
        }

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