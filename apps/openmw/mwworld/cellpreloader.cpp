#include "cellpreloader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <span>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <components/sceneutil/pagingwork.hpp>
#include <components/sceneutil/assetpaging.hpp>
#include <components/sceneutil/terraintargetreadiness.hpp>

#include <osg/Stats>

#include <components/debug/debuglog.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/debug/gameplaydiagnostics.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/loadinglistener/reporter.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/thread.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/bulletshapemanager.hpp>
#include <components/resource/animationdependencies.hpp>
#include <components/resource/animationpreparation.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/debug/v3gpumemory.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/manager.hpp>

#include "../mwrender/landmanager.hpp"
#include "../mwclass/npc.hpp"
#include <components/esm3/loadnpc.hpp>
#include <components/debug/resourcelifecycle.hpp>

#include "cellstore.hpp"
#include "class.hpp"

namespace MWWorld
{
    namespace
    {
        bool contains(std::span<const PositionCellGrid> positions, const PositionCellGrid& contained, float tolerance)
        {
            const float squaredTolerance = tolerance * tolerance;
            const auto predicate = [&](const PositionCellGrid& v) {
                return (contained.mPosition - v.mPosition).length2() < squaredTolerance
                    && contained.mCellBounds == v.mCellBounds;
            };
            return std::ranges::any_of(positions, predicate);
        }

        bool contains(
            std::span<const PositionCellGrid> container, std::span<const PositionCellGrid> contained, float tolerance)
        {
            const auto predicate = [&](const PositionCellGrid& v) { return contains(container, v, tolerance); };
            return std::ranges::all_of(contained, predicate);
        }
    }

    struct ListModelsVisitor
    {
        bool operator()(const MWWorld::ConstPtr& ptr)
        {
            ptr.getClass().getModelsToPreload(ptr, mOut);
            if (mAnimationSources && ptr.getType() == ESM::NPC::sRecordId)
            {
                try
                {
                    MWClass::Npc::getAnimationSourcesToPreload(
                        ptr, *mVFS, *mAnimationSources, Resource::MaximumAnimationPreloadSources);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Optional NPC animation snapshot failed: " << e.what();
                }
            }

            return true;
        }

        std::vector<VFS::Path::NormalizedView>& mOut;
        const VFS::Manager* mVFS = nullptr;
        std::vector<VFS::Path::Normalized>* mAnimationSources = nullptr;
    };

    /// Worker thread item: preload models in a cell.
    class PreloadItem : public SceneUtil::WorkItem
    {
    public:
        /// Constructor to be called from the main thread.
        explicit PreloadItem(MWWorld::CellStore* cell, Resource::SceneManager* sceneManager,
            Resource::BulletShapeManager* bulletShapeManager, Resource::KeyframeManager* keyframeManager,
            Terrain::World* terrain, MWRender::LandManager* landManager, bool preloadInstances,
            Resource::ResourceSystem* resourceSystem, bool useLegacyTerrain,
            Resource::PreloadAdmission::Reservation reservation, Resource::SpeculativeBudget::Job byteJob = {},
            Resource::SpeculativePriority priority = Resource::SpeculativePriority::Background,
            osg::ref_ptr<const Resource::AnimationPreparation> resume = {})
            : mIsExterior(cell->getCell()->isExterior())
            , mCellLocation(cell->getCell()->getExteriorCellLocation())
            , mCellId(cell->getCell()->getId())
            , mSceneManager(sceneManager)
            , mBulletShapeManager(bulletShapeManager)
            , mKeyframeManager(keyframeManager)
            , mTerrain(terrain)
            , mLandManager(landManager)
            , mPreloadInstances(preloadInstances)
            , mResourceSystem(resourceSystem)
            , mUseLegacyTerrain(useLegacyTerrain)
            , mReservation(std::move(reservation))
            , mByteJob(std::move(byteJob))
            , mPriority(priority)
            , mAbort(false)
            , mAnimationResume(std::move(resume))
        {
            Debug::HitchTrace::Scope resources("resource.preload_snapshot",
                Debug::HitchTrace::CpuTiming::Scoped);
            resources.retain();
            mResourceOriginSpan = resources.id();
            if (mUseLegacyTerrain)
                mTerrainView = mTerrain->createView();

            ListModelsVisitor visitor{ mMeshes };
            if (Resource::animationDependencyPreloadEnabled())
            {
                Debug::HitchTrace::Scope snapshot("animation.dependency_snapshot",
                    Debug::HitchTrace::CpuTiming::Scoped);
                snapshot.retain();
                mAnimationSnapshotSpan = snapshot.id();
                visitor.mVFS = mSceneManager->getVFS();
                visitor.mAnimationSources = &mAnimationSources;
                mAnimationVfsGeneration = visitor.mVFS->getIndexGeneration();
                mAnimationAdditionalSources = Settings::game().mUseAdditionalAnimSources;
                cell->forEachConst(visitor);
                snapshot.setMetric(0, "sources", mAnimationSources.size());
                snapshot.setMetric(1, "vfs_generation", mAnimationVfsGeneration);
                snapshot.setMetric(2, "additional_sources", mAnimationAdditionalSources);
                if (Resource::animationDependencyLifetimeEnabled() && mByteJob)
                {
                    if (mAnimationResume && !mAnimationResume->matches(
                            mAnimationVfsGeneration, mAnimationAdditionalSources, mAnimationSources))
                        mAnimationResume = nullptr;
                    mAnimationProgress = new Resource::AnimationPreparation(*mResourceSystem->speculativeBudget(),
                        Resource::ResourceSystem::speculativeSample(mResourceSystem), mPriority,
                        mAnimationVfsGeneration, mAnimationAdditionalSources, mAnimationSources,
                        mAnimationResume ? mAnimationResume->metadataBytes() : 0);
                }
            }
            else
                cell->forEachConst(visitor);
            resources.setMetric(0, "models", mMeshes.size());
            resources.setMetric(1, "animation_sources", mAnimationSources.size());
            resources.setMetric(2, "vfs_generation", mSceneManager->getVFS()->getIndexGeneration());
            if (mByteJob)
            {
                Resource::SpeculativeScope scope(mResourceSystem->speculativeBudget(),
                    &Resource::ResourceSystem::speculativeSample, mResourceSystem, mPriority);
                auto shell = std::uint64_t(mMeshes.capacity()) * sizeof(mMeshes.front()) + sizeof(*this)
                    + std::uint64_t(mAnimationSources.capacity()) * sizeof(VFS::Path::Normalized);
                for (const auto& source : mAnimationSources) shell += source.value().size();
                Resource::SpeculativeScope::Stage stage(shell, shell);
                mOwnerCharge = Resource::SpeculativeScope::track(mResourceSystem->speculativeBudget(), {this, 3}, shell, true);
            }
        }

        std::uint64_t retainedEstimate() const noexcept
        {
            const auto estimate = mRetainedEstimate.load(std::memory_order_acquire);
            // Conservative pending-release upper bound includes adopted hits.
            // The shared payload ledger still charges each object exactly once.
            const auto owned = mAnimationProgress ? mAnimationProgress->retainedBytes() : 0;
            return owned > UINT64_MAX - estimate ? UINT64_MAX : estimate + owned;
        }
        void pendingRelease() { if (mOwnerCharge) mOwnerCharge->pending(); }
        void abort() override { mAbort = true; }
        bool fullyPrepared() const noexcept { return mFullyPrepared.load(std::memory_order_acquire); }
        osg::ref_ptr<Resource::AnimationPreparation> animationProgress() const { return mAnimationProgress; }

        /// Preload work to be called from the worker thread.
        void doWork() override
        {
            // The immutable main-thread snapshot and this declared optional
            // worker boundary establish origin. A filename match alone cannot.
            Debug::ResourceLifecycle::PreparationScope preparation("cell_model_preload", mResourceOriginSpan);
            // Release on every exit, including abort and exception. Until work
            // starts the member also covers the queue and canceled-item lifetime.
            const auto reservation = std::move(mReservation);
            auto byteJob = std::move(mByteJob);
            Resource::SpeculativeScope scope(byteJob ? mResourceSystem->speculativeBudget() : nullptr,
                &Resource::ResourceSystem::speculativeSample, mResourceSystem, mPriority);
            struct Receipt
            {
                Resource::SpeculativeScope& scope;
                std::atomic<std::uint64_t>& estimate;
                ~Receipt() { estimate.store((std::max)(64 * Resource::SpeculativeBudget::MiB,
                    scope.retainedEstimate()), std::memory_order_release); }
            } receipt{scope, mRetainedEstimate};
            bool animationRetryable = true;
            struct PublishProgress
            {
                Resource::AnimationPreparation* progress;
                bool& retryable;
                ~PublishProgress() { if (progress) progress->complete(retryable); }
            } publish{mAnimationProgress.get(), animationRetryable};
            std::size_t resumed = 0;
            if (mAnimationProgress && mAnimationResume)
            {
                // Preserve completed ready work even if a later terrain/model
                // reservation yields this fresh job before animation discovery.
                resumed = mAnimationProgress->importReady(*mAnimationResume);
                mAnimationResume = nullptr;
            }
            if (mAbort)
                return;
            if (mAnimationProgress)
            {
                const auto sample = Resource::ResourceSystem::speculativeSample(mResourceSystem);
                if (!Resource::animationNewWorkAllowed(sample, mPriority))
                {
                    if (!Resource::animationOwnedWorkMayRemain(sample, mPriority == Resource::SpeculativePriority::NearFuture))
                        mAnimationProgress->cancel();
                    return;
                }
            }
            else if (mPriority == Resource::SpeculativePriority::Background
                && mResourceSystem->hostMemoryPressure() != Resource::HostMemoryPressure::Normal)
                return;
            if (!mAnimationProgress && mPriority == Resource::SpeculativePriority::NearFuture
                && mResourceSystem->hostMemoryPressure() == Resource::HostMemoryPressure::Critical)
                return;
            if (mIsExterior)
            {
                try
                {
                    Resource::SpeculativeScope::Stage terrainStage(128 * Resource::SpeculativeBudget::MiB,
                        32 * Resource::SpeculativeBudget::MiB);
                    if (mUseLegacyTerrain)
                        mTerrain->cacheCell(mTerrainView.get(), mCellLocation.mX, mCellLocation.mY);
                    mPreloadedObjects.insert(mLandManager->getLand(mCellLocation));
                }
                catch (const Resource::SpeculativeDeferred&) { return; }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to cache terrain for exterior cell " << mCellLocation << ": "
                                        << e.what();
                }
            }

            // Optional animation parsing needs the P1B byte/job admission lane.
            // Other profiles retain historical demand loading when unavailable.
            if (Resource::animationDependencyPreloadEnabled())
            {
                Debug::HitchTrace::Scope animationWork("animation.dependency_preload",
                    Debug::HitchTrace::CpuTiming::Scoped);
                animationWork.retain();
                animationWork.setLink(mAnimationSnapshotSpan);
                animationWork.setMetric(0, "sources", mAnimationSources.size());
                std::uint64_t preparedCount = 0;
                std::uint64_t failedCount = 0;
                std::uint64_t skippedCount = 0;
                std::uint64_t denialReasons = byteJob || mAnimationSources.empty() ? 0 : 256;
                std::size_t scanned = 0;
                if (byteJob && !mAnimationSources.empty())
                {
                    const VFS::Manager& vfs = *mSceneManager->getVFS();
                    std::set<VFS::Path::Normalized> prepared;
                    const auto beforeAnimations = scope.retainedEstimate();
                    const auto allowed = [&] {
                        const auto pressure = mAnimationProgress ? Resource::HostMemoryPressure::Normal
                            : mResourceSystem->hostMemoryPressure();
                        const auto sample = mAnimationProgress
                            ? Resource::ResourceSystem::speculativeSample(mResourceSystem) : Resource::OpenGlPressureSample{};
                        const bool pressureAllows = mAnimationProgress ? Resource::animationNewWorkAllowed(sample, mPriority)
                            : mPriority == Resource::SpeculativePriority::Background
                            ? pressure == Resource::HostMemoryPressure::Normal
                            : pressure != Resource::HostMemoryPressure::Critical;
                        // These bits describe why optional coverage stopped;
                        // they do not claim a later demand access missed cache.
                        const auto cancelled = mAbort.load() || (mAnimationProgress && mAnimationProgress->cancelled());
                        const auto generation = vfs.getIndexGeneration();
                        const auto retained = mAnimationProgress
                            ? Resource::MaximumAnimationPreloadEstimate - mAnimationProgress->remainingBytes()
                            : scope.retainedEstimate() - beforeAnimations;
                        if (cancelled) denialReasons |= 1;
                        if (!pressureAllows) denialReasons |= 2;
                        if (mAnimationVfsGeneration != generation) denialReasons |= 4;
                        if (prepared.size() >= Resource::MaximumAnimationPreloadDependencies) denialReasons |= 8;
                        if (retained >= Resource::MaximumAnimationPreloadEstimate) denialReasons |= 16;
                        return Resource::animationDependencyPreparationAllowed(cancelled, pressureAllows,
                            mAnimationVfsGeneration, generation, prepared.size(), retained);
                    };
                    const auto keepGoing = [&] {
                        if (++scanned > Resource::MaximumAnimationPreloadScannedPaths)
                        {
                            denialReasons |= 32;
                            return false;
                        }
                        return allowed();
                    };
                    const auto prepare = [&](VFS::Path::NormalizedView keyframe) {
                        if (!allowed()) return false;
                        if (keyframe.value().size() > 4096 || keyframe.extension() != VFS::Path::ExtensionView("kf")
                            || !vfs.exists(keyframe) || !prepared.insert(VFS::Path::Normalized(keyframe)).second)
                        {
                            ++skippedCount;
                            return true;
                        }
                        try
                        {
                            // KeyframeManager reserves its parse/build estimate
                            // before miss reads. Hits keep existing cache holders.
                            const auto remaining = mAnimationProgress ? mAnimationProgress->remainingBytes()
                                : Resource::MaximumAnimationPreloadEstimate - (scope.retainedEstimate() - beforeAnimations);
                            if (mAnimationProgress)
                            {
                                if (const auto* ready = mAnimationProgress->find(keyframe);
                                    ready && !mKeyframeManager->isReadyLeaseCurrent(keyframe, ready->mLease))
                                    mAnimationProgress->forget(keyframe);
                                if (!mAnimationProgress->find(keyframe))
                                {
                                    auto lease = mKeyframeManager->getReadyLease(keyframe, remaining);
                                    if (!mAnimationProgress->adopt(keyframe, std::move(lease)))
                                    { denialReasons |= 512; return false; }
                                }
                            }
                            else
                                mPreloadedObjects.insert(mKeyframeManager->get(keyframe, remaining));
                            ++preparedCount;
                        }
                        catch (const Resource::SpeculativeDeferred&)
                        {
                            denialReasons |= 64;
                            return false;
                        }
                        catch (const std::exception& e)
                        {
                            ++failedCount;
                            Log(Debug::Warning) << "Optional animation dependency \"" << keyframe
                                << "\" failed to preload: " << e.what();
                        }
                        return true;
                    };
                    for (const auto& source : mAnimationSources)
                    {
                        Debug::HitchTrace::Scope discovery("animation.dependency_discovery",
                            Debug::HitchTrace::CpuTiming::WallEnvelope, {}, source.value());
                        const auto primary = Resource::animationKeyframePath(source);
                        if (!keepGoing() || !prepare(primary) || (mAnimationAdditionalSources
                            && !mKeyframeManager->visitAdditionalSources(primary, prepare, keepGoing,
                                [&] { return Resource::ResourceSystem::speculativeSample(mResourceSystem); })))
                            break; // denied preparation never delays required activation
                    }
                }
                animationWork.setMetric(1, "scanned", scanned);
                animationWork.setMetric(2, "prepared", preparedCount);
                animationWork.setMetric(3, "denied", denialReasons != 0);
                Debug::HitchTrace::Scope outcome("animation.dependency_outcome",
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                outcome.retain();
                outcome.setLink(mAnimationSnapshotSpan);
                outcome.setMetric(0, "fallback_required", denialReasons != 0 || failedCount != 0);
                outcome.setMetric(1, "denial_reasons", denialReasons);
                outcome.setMetric(2, "failed", failedCount);
                outcome.setMetric(3, "skipped", skippedCount);
                animationRetryable = (denialReasons & (1 | 2 | 64 | 256)) != 0
                    && !(denialReasons & (4 | 8 | 16 | 32 | 512));
                if (mAnimationProgress)
                {
                    Debug::HitchTrace::Scope ownership("animation.ready_ownership");
                    ownership.retain();
                    ownership.setLink(mAnimationSnapshotSpan);
                    ownership.setMetric(0, "resumed", resumed);
                    ownership.setMetric(1, "ready", mAnimationProgress->size());
                    ownership.setMetric(2, "owned_estimate", Resource::MaximumAnimationPreloadEstimate
                        - mAnimationProgress->remainingBytes());
                    ownership.setMetric(3, "retryable", animationRetryable);
                }
            }

            VFS::Path::Normalized mesh;
            VFS::Path::Normalized kfname;
            std::set<VFS::Path::Normalized> v37PreloadedKeyframes;
            const bool v37CompanionKeyframePreload
                = static_cast<bool>(Settings::cells().mV37CompanionKeyframePreload);
            std::set<VFS::Path::Normalized> p1bMeshes;
            for (VFS::Path::NormalizedView path : mMeshes)
            {
                // These are speculative cache owners, not active gameplay.
                // Stop between assets under pressure, even during loading waits.
                if (mAbort)
                    return;
                const auto currentPressure = mResourceSystem->hostMemoryPressure();
                if ((mPriority == Resource::SpeculativePriority::Background
                        && currentPressure != Resource::HostMemoryPressure::Normal)
                    || (mPriority == Resource::SpeculativePriority::NearFuture
                        && currentPressure == Resource::HostMemoryPressure::Critical))
                    return;

                try
                {
                    const VFS::Manager& vfs = *mSceneManager->getVFS();
                    mesh = Misc::ResourceHelpers::correctMeshPath(path);
                    mesh = Misc::ResourceHelpers::correctActorModelPath(mesh, &vfs);

                    if (!vfs.exists(mesh))
                        continue;
                    if (byteJob && !p1bMeshes.insert(mesh).second) continue;
                    // Non-NIF plugin readers can swallow nested decode errors.
                    // Do not budget-abort through unknown plugins; let demand
                    // load them on the unchanged required path instead.
                    if (byteJob && mesh.extension() != VFS::Path::ExtensionView("nif")) continue;

                    constexpr VFS::Path::ExtensionView nif("nif");
                    const bool v37CheckKeyframe = mesh.extension() == nif
                        && (Misc::getFileName(mesh).starts_with('x') || v37CompanionKeyframePreload);
                    if (v37CheckKeyframe)
                    {
                        kfname = mesh;
                        constexpr VFS::Path::ExtensionView kf("kf");
                        kfname.changeExtension(kf);
                        if (vfs.exists(kfname) && v37PreloadedKeyframes.insert(kfname).second)
                            mPreloadedObjects.insert(mKeyframeManager->get(kfname));
                    }

                    // Outer shell/instance preparation estimate; nested image,
                    // NIF and template stages have their own reservations.
                    Resource::SpeculativeScope::Stage assetStage(32 * Resource::SpeculativeBudget::MiB,
                        8 * Resource::SpeculativeBudget::MiB);
                    if (Resource::v321CP2FairnessEnabled())
                        mPreloadedObjects.insert(mSceneManager->getTemplate(
                            mesh, true, Resource::V321CompileClass::GenericModel));
                    else
                        mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    if (mPreloadInstances)
                        mPreloadedObjects.insert(mBulletShapeManager->cacheInstance(mesh));
                    else
                        mPreloadedObjects.insert(mBulletShapeManager->getShape(mesh));
                    if (!mAbort)
                        mSceneManager->prepareInstance(mesh);
                }
                catch (const Resource::SpeculativeDeferred&) { return; }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to preload mesh \"" << path << "\" from cell " << mCellId << ": "
                                        << e.what();
                }
            }
            mFullyPrepared.store(true, std::memory_order_release);
        }

    private:
        bool mIsExterior;
        ESM::ExteriorCellLocation mCellLocation;
        ESM::RefId mCellId;
        std::vector<VFS::Path::NormalizedView> mMeshes;
        std::vector<VFS::Path::Normalized> mAnimationSources;
        std::uint64_t mAnimationVfsGeneration = 0;
        std::uint64_t mAnimationSnapshotSpan = 0;
        std::uint64_t mResourceOriginSpan = 0;
        bool mAnimationAdditionalSources = false;
        Resource::SceneManager* mSceneManager;
        Resource::BulletShapeManager* mBulletShapeManager;
        Resource::KeyframeManager* mKeyframeManager;
        Terrain::World* mTerrain;
        MWRender::LandManager* mLandManager;
        bool mPreloadInstances;
        Resource::ResourceSystem* mResourceSystem;
        bool mUseLegacyTerrain;
        Resource::PreloadAdmission::Reservation mReservation;
        Resource::SpeculativeBudget::Job mByteJob;
        Resource::SpeculativePriority mPriority = Resource::SpeculativePriority::Background;
        Resource::SpeculativeBudget::ChargePtr mOwnerCharge;
        std::atomic<std::uint64_t> mRetainedEstimate{64 * Resource::SpeculativeBudget::MiB};

        std::atomic<bool> mAbort;
        std::atomic<bool> mFullyPrepared{false};
        osg::ref_ptr<const Resource::AnimationPreparation> mAnimationResume;
        osg::ref_ptr<Resource::AnimationPreparation> mAnimationProgress;

        osg::ref_ptr<Terrain::View> mTerrainView;

        // keep a ref to the loaded objects to make sure it stays loaded as long as this cell is in the preloaded state
        std::set<osg::ref_ptr<const osg::Object>> mPreloadedObjects;
    };

    class TerrainPreloadItem : public SceneUtil::WorkItem
    {
    public:
        explicit TerrainPreloadItem(const std::vector<osg::ref_ptr<Terrain::View>>& views, Terrain::World* world,
            Resource::ResourceSystem* resourceSystem, std::span<const PositionCellGrid> preloadPositions,
            bool cancellableOptimization = false, bool readinessSplit = false, std::uint64_t hitchJob = 0,
            bool currentFirst = false)
            : mAbort(false)
            , mCancellableOptimization(cancellableOptimization)
            , mReadinessSplit(cancellableOptimization && readinessSplit)
            , mCurrentFirst(mReadinessSplit && currentFirst)
            , mTerrainViews(views)
            , mWorld(world)
            , mWorldspace(world->getWorldspace())
            , mResourceSystem(resourceSystem)
            , mPreloadPositions(preloadPositions.begin(), preloadPositions.end())
            , mTargetReadiness(mCurrentFirst ? std::make_shared<SceneUtil::TerrainTargetReadiness>(preloadPositions.size()) : nullptr)
            , mHitchJob(hitchJob)
        {
            mOptionalRetry.generation = optionalGeneration();
            if (Debug::GameplayDiagnostics::enabled() || Debug::RuntimeDiagnostics::enabled())
                mQueued = Debug::GameplayDiagnostics::Clock::now();
        }

        // Constructed only after the predecessor worker published Done. The
        // completed weak views/readiness remain visible; only their optional
        // strong cache preparation is revisited, with the finite same-cohort
        // ledger. No required view is reset or replayed on this path.
        TerrainPreloadItem(const TerrainPreloadItem& previous,std::uint64_t hitchJob)
            : mAbort(false)
            , mCancellableOptimization(previous.mCancellableOptimization)
            , mReadinessSplit(previous.mReadinessSplit)
            , mCurrentFirst(previous.mCurrentFirst)
            , mSucceeded(true)
            , mTerrainViews(previous.mTerrainViews)
            , mWorld(previous.mWorld)
            , mWorldspace(previous.mWorldspace)
            , mResourceSystem(previous.mResourceSystem)
            , mPreloadPositions(previous.mPreloadPositions)
            , mTargetReadiness(previous.mTargetReadiness)
            , mHitchJob(hitchJob)
            , mOptionalRetry(previous.mOptionalRetry)
            , mOptionalRetryOnly(true)
        {
            mRequiredReporter.complete();
            if (Debug::GameplayDiagnostics::enabled() || Debug::RuntimeDiagnostics::enabled())
                mQueued = Debug::GameplayDiagnostics::Clock::now();
        }

        void doWork() override
        {
            Debug::HitchTrace::Scope hitchWork("terrain.job_work", Debug::HitchTrace::CpuTiming::Scoped);
            hitchWork.retain();
            hitchWork.setLink(mHitchJob);
            hitchWork.setMetric(0, "targets", mPreloadPositions.size());
            hitchWork.setMetric(1, "readiness_split", mReadinessSplit);
            const bool measure = Debug::GameplayDiagnostics::enabled() || Debug::RuntimeDiagnostics::enabled();
            const auto start = measure ? Debug::GameplayDiagnostics::Clock::now()
                                       : Debug::GameplayDiagnostics::Clock::time_point{};
            if (measure) mQueueMs = std::chrono::duration<double, std::milli>(start - mQueued).count();

            const auto prepare = [&](Loading::Reporter& reporter) {
                for (unsigned int i = 0; i < mTerrainViews.size() && i < mPreloadPositions.size() && !mAbort; ++i)
                {
                    Debug::HitchTrace::Scope hitchView("terrain.view_prepare", Debug::HitchTrace::CpuTiming::Scoped);
                    hitchView.setLink(mHitchJob);
                    hitchView.setMetric(0, "target_index", i);
                    hitchView.setMetric(1, "required_phase", mReadinessSplit);
                    SceneUtil::PagingWorkScope::checkpoint();
                    mTerrainViews[i]->reset();
                    mWorld->preload(mTerrainViews[i], mPreloadPositions[i].mPosition, mPreloadPositions[i].mCellBounds,
                        mAbort, reporter);
                }
            };
            const auto prepareRequired = [&](Loading::Reporter& reporter, SceneUtil::PagingWorkScope::Phase phase) {
                using Epoch = std::pair<std::uint64_t, std::uint64_t>;
                const auto readEpoch = [&]() -> std::optional<Epoch> {
                    if (mWorld->getWorldspace() != mWorldspace) return std::nullopt;
                    return Epoch{mResourceSystem->getVFS()->getIndexGeneration(),
                        mResourceSystem->getSceneManager()->getShaderManager().assetGeneration()};
                };
                try
                {
                    const bool ready = SceneUtil::prepareFreshRequiredPaging(mAbort, readEpoch, [&] {
                        SceneUtil::PagingWorkScope scope(&mAbort, phase);
                        prepare(reporter);
                    }, [&](const Epoch& before, const Epoch& after, unsigned retry) {
                        Log(Debug::Info) << "Required terrain source epoch changed during private preparation; retry="
                            << retry << " maximum_attempts=" << SceneUtil::RequiredPagingMaximumAttempts
                            << " vfs_before=" << before.first << " vfs_after=" << after.first
                            << " shader_before=" << before.second << " shader_after=" << after.second
                            << "; rebuilding complete private views before required publication";
                    });
                    mSucceeded.store(ready, std::memory_order_release);
                }
                catch (...)
                {
                    // Only source-epoch cancellation is retried. Actual loader
                    // errors retain their original exception for the main thread.
                    mRequiredFailure = std::current_exception();
                }
            };

            if (mOptionalRetryOnly)
            {
                // A queued retry grants no permission to use stale source/world
                // data. Cancellation keeps already-published weak readiness.
                if (optionalGeneration() == mOptionalRetry.generation && matchesWorld(mWorld)
                    && !mAbort.load(std::memory_order_acquire))
                    prepareOptionalStrong(measure);
                else
                { mOptionalRetry.cancel(); ++SceneUtil::AssetPaging::Counters::tailCancelled; }
                completeLoadingReporter();
            }
            else if (mCurrentFirst)
                prepareCurrentFirst(measure);
            else if (mCancellableOptimization && mReadinessSplit)
            {
                const auto readyStart = Debug::GameplayDiagnostics::Clock::now();
                {
                    Debug::HitchTrace::Scope hitchRequired("terrain.required_prepare", Debug::HitchTrace::CpuTiming::Scoped);
                    hitchRequired.setLink(mHitchJob);
                    prepareRequired(mRequiredReporter, SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
                }
                if (measure)
                    mReadinessMs = std::chrono::duration<double, std::milli>(
                        Debug::GameplayDiagnostics::Clock::now() - readyStart).count();

                // Publish only the required phase to syncTerrainLoad. The work
                // item deliberately remains alive while the stronger V3.13
                // cache upgrade continues on the worker.
                {
                    Debug::HitchTrace::Scope publication("terrain.required_reporter_complete");
                    publication.retain();
                    publication.setLink(mHitchJob);
                    publication.setMetric(0, "succeeded", mSucceeded.load(std::memory_order_acquire));
                    publication.setMetric(1, "aborted", mAbort.load());
                    publication.setMetric(2, "failed", bool(mRequiredFailure));
                    mRequiredReporter.complete();
                }

                if (mSucceeded.load(std::memory_order_acquire) && !mAbort.load(std::memory_order_relaxed))
                    prepareOptionalStrong(measure);
            }
            else if (mCancellableOptimization)
            {
                Debug::HitchTrace::Scope hitchRequired("terrain.required_prepare", Debug::HitchTrace::CpuTiming::Scoped);
                hitchRequired.setLink(mHitchJob);
                prepareRequired(mLoadingReporter, SceneUtil::PagingWorkScope::Phase::Full);
                completeLoadingReporter();
            }
            else
            {
                Debug::HitchTrace::Scope hitchRequired("terrain.required_prepare", Debug::HitchTrace::CpuTiming::Scoped);
                hitchRequired.setLink(mHitchJob);
                prepare(mLoadingReporter);
                completeLoadingReporter();
            }

            if (measure)
                mWorkMs = std::chrono::duration<double, std::milli>(
                    Debug::GameplayDiagnostics::Clock::now() - start).count();
            if (Debug::RuntimeDiagnostics::enabled())
                Debug::RuntimeDiagnostics::recordEvent("terrain_job", "legacy_terrain_preload", {}, {
                    {"job", reinterpret_cast<std::uintptr_t>(this)},
                    {"queue_us", static_cast<std::uint64_t>((std::max)(0.0, mQueueMs.load()) * 1000)},
                    {"work_us", static_cast<std::uint64_t>((std::max)(0.0, mWorkMs.load()) * 1000)},
                    {"readiness_us", static_cast<std::uint64_t>((std::max)(0.0, mReadinessMs.load()) * 1000)},
                    {"optional_us", static_cast<std::uint64_t>((std::max)(0.0, mOptionalMs.load()) * 1000)},
                    {"optional_requests", mOptionalUpgradeRequests.load(std::memory_order_relaxed)},
                    {"optional_deferred", mOptionalDeferred.load(std::memory_order_relaxed)},
                    {"views", mPreloadPositions.size()}, {"aborted", mAbort.load()},
                    {"readiness_split", mReadinessSplit} });
        }

        void abort() override
        {
            Debug::HitchTrace::Scope cancellation("terrain.cancel_request");
            cancellation.retain();
            cancellation.setLink(mHitchJob);
            cancellation.setMetric(0, "already_aborted", mAbort.load());
            cancellation.setMetric(1, "required_ready", readinessComplete());
            mAbort = true;
        }
        std::uint64_t hitchJob() const noexcept { return mHitchJob; }
        void traceLoadedPublication(std::size_t targets) noexcept
        {
            // The legacy update path refreshes its cache timestamp repeatedly.
            // Record the first visibility publication once per stable job ID.
            if (!mHitchJob || mHitchLoadedPublication) return;
            mHitchLoadedPublication = true;
            Debug::HitchTrace::Scope publication("terrain.loaded_positions_publish");
            publication.retain();
            publication.setLink(mHitchJob);
            publication.setMetric(0, "targets", targets);
            publication.setMetric(1, "readiness_split", mReadinessSplit);
        }
        bool succeeded() const noexcept
        { return !mCancellableOptimization || mSucceeded.load(std::memory_order_acquire); }
        bool readinessSplit() const noexcept { return mReadinessSplit; }
        bool currentFirst() const noexcept { return mCurrentFirst; }
        bool matchesWorld(Terrain::World* world) const
        { return world == mWorld && world && world->getWorldspace() == mWorldspace; }
        bool optionalSourcesMatch() const noexcept
        { return optionalGeneration() == mOptionalRetry.generation; }
        bool optionalRetryDue(std::uint64_t now) const noexcept
        {
            const bool due = isDone() && mReadinessSplit && succeeded() && !mAbort.load(std::memory_order_acquire)
                && (mOptionalRetry.generation.mask & SceneUtil::AssetPaging::Mask)
                && mOptionalRetry.due(optionalGeneration(),now);
            if (!due || now < mOptionalAdmissionPollAfter) return false;
            auto* budget = mResourceSystem->speculativeBudget();
            if (budget && budget->experimentEnabled()
                && budget->experimentJobReceipt(Resource::ResourceSystem::speculativeSample(mResourceSystem)).reasons)
            {
                // A denied permit queues no worker and copies no views/graph.
                // Poll at the existing bounded cooldown; do not exhaust the
                // finite construction ledger before a construction is admitted.
                mOptionalAdmissionPollAfter = now <= UINT64_MAX-SceneUtil::AssetPaging::RetryDelayMilliseconds
                    ? now+SceneUtil::AssetPaging::RetryDelayMilliseconds : UINT64_MAX;
                return false;
            }
            mOptionalAdmissionPollAfter = 0;
            return true;
        }
        std::optional<std::size_t> targetIndex(const PositionCellGrid& position) const
        {
            for (std::size_t i = 0; i < mPreloadPositions.size(); ++i)
                if (contains(std::span(&mPreloadPositions[i], 1), position, Constants::CellSizeInUnits)) return i;
            return std::nullopt;
        }
        bool canServe(std::span<const PositionCellGrid> positions) const
        {
            for (const auto& position : positions)
            {
                const auto index = targetIndex(position);
                if (!index || !mTargetReadiness->canServe(*index)) return false;
            }
            return true;
        }
        std::vector<PositionCellGrid> completedTargets() const
        {
            std::vector<PositionCellGrid> result;
            for (std::size_t index : mTargetReadiness->completed()) result.push_back(mPreloadPositions[index]);
            return result;
        }
        void waitTarget(std::size_t index, Loading::Listener& listener)
        {
            Debug::HitchTrace::Scope wait("terrain.target_wait", Debug::HitchTrace::CpuTiming::Scoped);
            wait.retain(); wait.setLink(mHitchJob); wait.setMetric(0, "target_index", index);
            mTargetReadiness->wait(index, listener);
        }
        bool readinessComplete() const noexcept
        { return mReadinessSplit && mSucceeded.load(std::memory_order_acquire); }
        bool takeReadinessPublication() noexcept
        {
            return readinessComplete() && !mReadinessPublished.exchange(true, std::memory_order_acq_rel);
        }

        void wait(Loading::Listener& listener)
        {
            if (mCurrentFirst)
            {
                // Without an explicit demanded position no target can stand in
                // for the rest. Await all requested complete views or fail closed.
                for (std::size_t index = 0; index < mPreloadPositions.size(); ++index) waitTarget(index, listener);
                return;
            }
            Debug::HitchTrace::Scope hitchWait("terrain.required_wait", Debug::HitchTrace::CpuTiming::Scoped);
            hitchWait.retain();
            hitchWait.setLink(mHitchJob);
            hitchWait.setMetric(0, "readiness_split", mReadinessSplit);
            hitchWait.setMetric(1, "done_at_entry", isDone());
            Debug::RuntimeDiagnostics::Operation runtimeOperation("required_terrain_wait");
            if (Debug::RuntimeDiagnostics::enabled())
                Debug::RuntimeDiagnostics::recordEvent("wait_dependency", "required_terrain_wait", {},
                    {{"job", reinterpret_cast<std::uintptr_t>(this)}, {"views", mPreloadPositions.size()},
                     {"readiness_split", mReadinessSplit}});
            Debug::GameplayDiagnostics::Operation operation("terrain_wait");

            if (mReadinessSplit)
            {
                {
                    Debug::HitchTrace::Scope reporterWait("terrain.required_reporter_wait", Debug::HitchTrace::CpuTiming::Scoped);
                    reporterWait.setLink(mHitchJob);
                    mRequiredReporter.wait(listener);
                }
                // Required-phase state is immutable after reporter completion.
                // Do not join the optional strong-upgrade tail.
                if (mRequiredFailure) std::rethrow_exception(mRequiredFailure);
                if (!succeeded())
                    throw std::runtime_error("Required terrain preparation was cancelled; no partial view published");
            }
            else
            {
                {
                    Debug::HitchTrace::Scope reporterWait("terrain.loading_reporter_wait", Debug::HitchTrace::CpuTiming::Scoped);
                    reporterWait.setLink(mHitchJob);
                    mLoadingReporter.wait(listener);
                }
                if (mCancellableOptimization)
                {
                    // Reporter completion precedes WorkQueue::signalDone.
                    {
                        Debug::HitchTrace::Scope jobJoin("terrain.worker_completion_wait", Debug::HitchTrace::CpuTiming::Scoped);
                        jobJoin.setLink(mHitchJob);
                        waitTillDone();
                    }
                    if (mRequiredFailure) std::rethrow_exception(mRequiredFailure);
                    if (!succeeded())
                        throw std::runtime_error("Required terrain preparation was cancelled; no partial view published");
                }
            }

            if (Debug::GameplayDiagnostics::enabled())
                Debug::GameplayDiagnostics::recordEvent("terrain_preload_work", {
                    {"queue_ms", std::to_string(mQueueMs.load())},
                    {"work_ms", std::to_string(mWorkMs.load())},
                    {"readiness_ms", std::to_string(mReadinessMs.load())},
                    {"optional_ms", std::to_string(mOptionalMs.load())},
                    {"optional_requests", std::to_string(mOptionalUpgradeRequests.load())},
                    {"optional_deferred", std::to_string(mOptionalDeferred.load())},
                    {"views", std::to_string(mPreloadPositions.size())},
                    {"aborted", std::to_string(mAbort.load())},
                    {"readiness_split", std::to_string(mReadinessSplit)}}, true);
        }

    private:
        SceneUtil::AssetPaging::OptionalTailRetry::Generation optionalGeneration() const noexcept
        {
            const unsigned mask = Resource::AssetPolicy::mask() & SceneUtil::AssetPaging::Mask;
            if (!mask) return {mWorld,0,0,0};
            return {mWorld,mResourceSystem->getVFS()->getIndexGeneration(),
                mResourceSystem->getSceneManager()->getShaderManager().assetGeneration(),
                mask};
        }

        void prepareOptionalStrong(bool measure)
        {
            Debug::HitchTrace::Scope optional("terrain.optional_prepare",Debug::HitchTrace::CpuTiming::Scoped);
            optional.setLink(mHitchJob);
            optional.setMetric(0,"tail_retry",mOptionalRetryOnly);
            optional.setMetric(1,"tail_attempt",mOptionalRetry.attempts+1);
            const auto start = measure ? Debug::GameplayDiagnostics::Clock::now()
                                       : Debug::GameplayDiagnostics::Clock::time_point{};
            Loading::Reporter reporter;
            auto* budget = mResourceSystem->speculativeBudget();
            bool admitted = true, deferred = false, spentAttempt = false;
            if (!budget && mResourceSystem->openGlHostMemoryBudgetEnabled())
            {
                const auto sample = Resource::ResourceSystem::speculativeSample(mResourceSystem);
                admitted = sample.generation && sample.decision.state != Resource::OpenGlPressure::Critical
                    && sample.decision.state != Resource::OpenGlPressure::Degraded;
            }
            else if (mResourceSystem->hostMemoryPressure() == Resource::HostMemoryPressure::Critical)
                admitted = false;
            if (admitted && (mOptionalRetry.generation.mask & SceneUtil::AssetPaging::Mask)
                && budget && budget->experimentEnabled())
            {
                const auto permit = budget->experimentJobReceipt(Resource::ResourceSystem::speculativeSample(mResourceSystem));
                if (permit.reasons)
                {
                    admitted = false;
                    SceneUtil::AssetPaging::cpuRefusal(mOptionalRetry.generation.mask,
                        SceneUtil::AssetPaging::CpuStage::Outer,Resource::SpeculativeDeferred(permit));
                    SceneUtil::AssetPaging::each(mOptionalRetry.generation.mask,[](SceneUtil::AssetPaging::LaneCounters& value) {
                        ++value.outerCpuDenied;
                    });
                    SceneUtil::AssetPaging::refuse(mOptionalRetry.generation.mask,SceneUtil::AssetPaging::Gate::Capacity);
                }
            }
            if (admitted)
            {
                // The optional tail remains serialized by this terrain owner;
                // NearFuture byte/headroom gates and the outer128MiB reservation
                // are unchanged. No P1 job slot or demand wait is introduced.
                Resource::SpeculativeScope speculativeScope(budget,
                    &Resource::ResourceSystem::speculativeSample,mResourceSystem,
                    Resource::SpeculativePriority::NearFuture);
                std::unique_ptr<SceneUtil::AssetPaging::OptionalUpgradeScope> scope;
                try
                {
                    scope = std::make_unique<SceneUtil::AssetPaging::OptionalUpgradeScope>(&mAbort);
                    const auto prepare = [&](std::size_t index) {
                        if (mAbort.load(std::memory_order_relaxed)) return;
                        SceneUtil::PagingWorkScope::checkpoint();
                        if (mOptionalRetry.generation.mask && (!optionalSourcesMatch() || !matchesWorld(mWorld)))
                            throw SceneUtil::PagingWorkCancelled{};
                        mOptionalUpgradeRequests.fetch_add(mWorld->preloadStrongUpgrade(mTerrainViews[index],
                            mPreloadPositions[index].mPosition,mPreloadPositions[index].mCellBounds,mAbort,reporter),
                            std::memory_order_relaxed);
                    };
                    if (mCurrentFirst)
                        for (std::size_t index : mTargetReadiness->completed()) prepare(index);
                    else
                        for (std::size_t index=0;index<mTerrainViews.size() && index<mPreloadPositions.size();++index)
                            prepare(index);
                    deferred = scope->needsRetry();
                    spentAttempt = scope->spentAttempt() && !scope->waitingForAdmission();
                    mOptionalCompleted.store(!mAbort.load() && !deferred,std::memory_order_release);
                }
                catch (const Resource::SpeculativeDeferred& error)
                {
                    deferred = true;
                    spentAttempt = (scope && scope->spentAttempt() && !scope->waitingForAdmission())
                        || !SceneUtil::AssetPaging::admissionWaiting(error);
                }
                catch (const SceneUtil::PagingWorkCancelled&)
                { mOptionalRetry.cancel(); ++SceneUtil::AssetPaging::Counters::tailCancelled; }
                catch (const std::exception& e)
                { Log(Debug::Warning) << "Optional paging optimization failed after target readiness: " << e.what(); }
                catch (...)
                { Log(Debug::Warning) << "Optional paging optimization failed after target readiness"; }
            }
            else deferred = true;
            mOptionalDeferred.store(deferred,std::memory_order_release);
            if (mOptionalRetry.generation.mask && mReadinessSplit)
            {
                ++SceneUtil::AssetPaging::Counters::tailAttempts;
                mOptionalRetry.finishAttempt(deferred && !mAbort.load(std::memory_order_acquire),
                    SceneUtil::AssetPaging::milliseconds(),spentAttempt);
                if (deferred) ++SceneUtil::AssetPaging::Counters::tailDeferred;
                if (mOptionalRetry.exhausted) ++SceneUtil::AssetPaging::Counters::tailExhausted;
            }
            reporter.complete();
            if (measure) mOptionalMs = std::chrono::duration<double,std::milli>(
                Debug::GameplayDiagnostics::Clock::now()-start).count();
        }

        void prepareCurrentFirst(bool measure)
        {
            // Serial private views, per-target publication. Future weak views
            // still use RequiredReadiness geometry, but are no longer a
            // dependency of the first demanded view's reporter.
            while (!mAbort.load(std::memory_order_relaxed))
            {
                const auto next = mTargetReadiness->nextPending();
                if (!next) break;
                const std::size_t index = *next;
                const auto start = measure ? Debug::GameplayDiagnostics::Clock::now()
                                           : Debug::GameplayDiagnostics::Clock::time_point{};
                const bool ready = mTargetReadiness->run(index, [&](Loading::Reporter& reporter) {
                    const auto prepareView = [&] {
                        Debug::HitchTrace::Scope view("terrain.view_prepare", Debug::HitchTrace::CpuTiming::Scoped);
                        view.setLink(mHitchJob); view.setMetric(0, "target_index", index);
                        view.setMetric(1, "required_phase", 1);
                        view.setMetric(2, "demanded_target", index == 0 || mTargetReadiness->demanded(index));
                        SceneUtil::PagingWorkScope scope(&mAbort, SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
                        SceneUtil::PagingWorkScope::checkpoint();
                        mTerrainViews.at(index)->reset();
                        mWorld->preload(mTerrainViews[index], mPreloadPositions[index].mPosition,
                            mPreloadPositions[index].mCellBounds, mAbort, reporter);
                        return !mAbort.load(std::memory_order_acquire);
                    };
                    try
                    {
                        // Demand never consumes a speculative reservation.
                        if (index == 0 || mTargetReadiness->demanded(index)) return prepareView();
                        Resource::SpeculativeBudget* budget = mResourceSystem->speculativeBudget();
                        if (!budget && mResourceSystem->openGlHostMemoryBudgetEnabled())
                        {
                            const auto sample = Resource::ResourceSystem::speculativeSample(mResourceSystem);
                            if (!sample.generation || sample.decision.state == Resource::OpenGlPressure::Critical
                                || sample.decision.state == Resource::OpenGlPressure::Degraded)
                            { mOptionalDeferred.store(true, std::memory_order_release); return false; }
                        }
                        else if (mResourceSystem->hostMemoryPressure() == Resource::HostMemoryPressure::Critical)
                        { mOptionalDeferred.store(true, std::memory_order_release); return false; }
                        Resource::SpeculativeScope speculativeScope(budget,
                            &Resource::ResourceSystem::speculativeSample, mResourceSystem,
                            Resource::SpeculativePriority::NearFuture);
                        Resource::SpeculativeScope::Stage stage(128 * Resource::SpeculativeBudget::MiB, 0);
                        return prepareView();
                    }
                    catch (const Resource::SpeculativeDeferred&)
                    { mOptionalDeferred.store(true, std::memory_order_release); return false; }
                    catch (const SceneUtil::PagingWorkCancelled&) { return false; }
                }, [&](bool completed) noexcept {
                    // This receipt precedes immutable status visibility and the
                    // reporter wake. It is not the reporter's completion wall.
                    Debug::HitchTrace::Scope publication("terrain.target_readiness_publication",
                        Debug::HitchTrace::CpuTiming::WallEnvelope);
                    publication.retain(); publication.setLink(mHitchJob);
                    publication.setMetric(0, "target_index", index);
                    publication.setMetric(1, "ready", completed);
                    publication.setMetric(2, "demanded", index == 0 || mTargetReadiness->demanded(index));
                    if (index == 0)
                    {
                        if (measure) mReadinessMs = std::chrono::duration<double, std::milli>(
                            Debug::GameplayDiagnostics::Clock::now() - start).count();
                        mSucceeded.store(completed, std::memory_order_release);
                    }
                });
                if (!ready) break;
            }
            mTargetReadiness->cancelPending();
            if (mAbort.load(std::memory_order_acquire) || !mSucceeded.load(std::memory_order_acquire)) return;

            prepareOptionalStrong(measure);
        }

        void completeLoadingReporter()
        {
            Debug::HitchTrace::Scope publication("terrain.loading_reporter_complete");
            publication.retain();
            publication.setLink(mHitchJob);
            publication.setMetric(0, "publish_eligible", succeeded());
            publication.setMetric(1, "aborted", mAbort.load());
            publication.setMetric(2, "failed", bool(mRequiredFailure));
            mLoadingReporter.complete();
        }

        std::atomic<bool> mAbort;
        const bool mCancellableOptimization;
        const bool mReadinessSplit;
        const bool mCurrentFirst;
        std::atomic<bool> mSucceeded{false};
        std::atomic<bool> mReadinessPublished{false};
        std::exception_ptr mRequiredFailure;
        std::vector<osg::ref_ptr<Terrain::View>> mTerrainViews;
        Terrain::World* mWorld;
        const ESM::RefId mWorldspace;
        Resource::ResourceSystem* mResourceSystem;
        std::vector<PositionCellGrid> mPreloadPositions;
        std::shared_ptr<SceneUtil::TerrainTargetReadiness> mTargetReadiness;
        const std::uint64_t mHitchJob;
        bool mHitchLoadedPublication = false;
        Loading::Reporter mLoadingReporter;
        Loading::Reporter mRequiredReporter;
        Debug::GameplayDiagnostics::Clock::time_point mQueued{};
        std::atomic<double> mQueueMs{0}, mWorkMs{0}, mReadinessMs{0}, mOptionalMs{0};
        std::atomic<std::uint64_t> mOptionalUpgradeRequests{0};
        std::atomic<bool> mOptionalDeferred{false}, mOptionalCompleted{false};
        SceneUtil::AssetPaging::OptionalTailRetry mOptionalRetry;
        mutable std::uint64_t mOptionalAdmissionPollAfter = 0; // main-thread checks only after Done
        const bool mOptionalRetryOnly = false;
    };

    /// Worker thread item: update the resource system's cache, effectively deleting unused entries.
    class UpdateCacheItem : public SceneUtil::WorkItem
    {
    public:
        UpdateCacheItem(Resource::ResourceSystem* resourceSystem, double referenceTime, bool idlePriority,
            std::vector<osg::ref_ptr<SceneUtil::WorkItem>> releasedPreloads = {})
            : mReferenceTime(referenceTime)
            , mResourceSystem(resourceSystem)
            , mIdlePriority(idlePriority)
            , mReleasedPreloads(std::move(releasedPreloads))
        {
        }

        void doWork() override
        {
            if (mIdlePriority)
                Misc::setCurrentThreadIdlePriority();
            // Release cell pinning references here, not on the main thread.
            // Only completed work items are transferred; no new wait/barrier.
            {
                Debug::HitchTrace::Scope holders("resource.preload_holder_release");
                holders.setMetric(0, "work_items", mReleasedPreloads.size());
                mReleasedPreloads.clear();
            }
            mResourceSystem->updateCache(mReferenceTime);
        }

    private:
        double mReferenceTime;
        Resource::ResourceSystem* mResourceSystem;
        bool mIdlePriority;
        std::vector<osg::ref_ptr<SceneUtil::WorkItem>> mReleasedPreloads;
    };

    CellPreloader::CellPreloader(Resource::ResourceSystem* resourceSystem,
        Resource::BulletShapeManager* bulletShapeManager, Terrain::World* terrain, MWRender::LandManager* landManager,
        bool useLegacyTerrain)
        : mResourceSystem(resourceSystem)
        , mBulletShapeManager(bulletShapeManager)
        , mTerrain(terrain)
        , mLandManager(landManager)
        , mExpiryDelay(0.0)
        , mPreloadInstances(true)
        , mUseLegacyTerrain(useLegacyTerrain)
        , mCancellablePagingOptimization(useLegacyTerrain && Settings::cells().mOpimizedMWPagingOptimizer)
        , mPagingReadinessSplit(mCancellablePagingOptimization
            && static_cast<bool>(Settings::cells().mOpimizedMWPagingReadinessSplit)
            && static_cast<int>(Settings::cells().mV313ChunkQualityMode) > 0
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0)
        , mTerrainCurrentFirst(mPagingReadinessSplit && [] {
            const char* value = std::getenv("OPENMW_HITCH_TERRAIN_CURRENT_FIRST");
            return value && std::string_view(value) == "1";
        }())
        , mLastResourceCacheUpdate(0.0)
        , mLoadedTerrainTimestamp(0.0)
    {
        setColdRetention(mResourceSystem->speculativeBudget() && Resource::ramRetentionEnabled(1),
            mResourceSystem->speculativeBudget() && Resource::ramRetentionEnabled(4));
        if (static_cast<bool>(Settings::cells().mV316IdleResourceSweep) || mResourceSystem->speculativeBudget())
            mV316ResourceSweepQueue = new SceneUtil::WorkQueue(1);
    }

    CellPreloader::~CellPreloader()
    {
        if (mColdRetention)
            Log(Debug::Info) << "OptimizedMW cold preload owners: retired=" << mColdRetired
                << " indexed=" << mColdRetention->size() << " index_available=" << mColdRetention->available()
                << " base_retention=" << mColdBaseRetention << " early_headroom=" << mCoordinateColdHeadroom;
        clearAllTasks();
    }

    void CellPreloader::preload(CellStore& cell, double timestamp, bool nearFuture)
    {
        const bool p1b = mResourceSystem->speculativeBudget() != nullptr;
        const bool lifetime = p1b && Resource::animationDependencyLifetimeEnabled();
        if (lifetime && mAnimationTarget == &cell)
            nearFuture = true;
        const auto priority = nearFuture
            ? Resource::SpeculativePriority::NearFuture : Resource::SpeculativePriority::Background;
        if (p1b && mResourceSystem->pendingReleases() && !nearFuture)
            return;
        // Ordinary speculation yields immediately under host pressure. The
        // single closest not-yet-preloaded exterior cell may continue through
        // P1B's bounded NearFuture lane; its own sample/byte checks still deny
        // critical, degraded or insufficient-headroom work.
        if (!nearFuture && mResourceSystem->hostMemoryPressure() != Resource::HostMemoryPressure::Normal)
            return;
        if (!mWorkQueue)
        {
            Log(Debug::Error) << "Error: can't preload, no work queue set";
            return;
        }
        if (cell.getState() == CellStore::State_Unloaded)
        {
            Log(Debug::Error) << "Error: can't preload objects for unloaded cell";
            return;
        }

        PreloadMap::iterator found = mPreloadCells.find(&cell);
        if (found != mPreloadCells.end())
        {
            if (lifetime && mAnimationTarget == &cell && found->second.mWorkItem)
            {
                const auto& previous = static_cast<const PreloadItem&>(*found->second.mWorkItem);
                const auto progress = previous.animationProgress();
                if ((!mAnimationTicket || mAnimationTicketCell == &cell) && progress && !progress->cancelled())
                    retainAnimationProgress(&cell, progress);
                if (!found->second.mRetired && previous.isDone() && progress && progress->retryable()
                    && timestamp >= mAnimationRetryAt)
                    found->second.mRetired = true;
            }
            // A retired or incomplete owner is not a warm ready cell.
            if (found->second.mRetired)
            {
                if (!lifetime) return;
                retireCompletedPreloads();
                found = mPreloadCells.find(&cell);
                if (found != mPreloadCells.end()) return;
            }
            else
            {
            // already preloaded, nothing to do other than updating the timestamp
            found->second.mTimeStamp = timestamp;
            if (mColdRetention) mColdRetention->request(&cell, timestamp, nearFuture);
            return;
            }
        }

        const auto sample = p1b ? Resource::ResourceSystem::speculativeSample(mResourceSystem)
                                : Resource::OpenGlPressureSample{};
        if (lifetime && !Resource::animationNewWorkAllowed(sample, priority)) return;
        if (lifetime && nearFuture && mAnimationTicketCell == &cell && mAnimationTicket
            && mAnimationTicket->published() && !mAnimationTicket->retryable()
            && sample.decision.state == Resource::OpenGlPressure::Caution)
            return; // keep useful KF ownership; do not repeatedly rebuild broad model owners

        static const bool legacyAdmission = std::getenv("OPENMW_V4_LEGACY_PRELOAD_ADMISSION_CONTROL") != nullptr;
        auto reservation = (p1b || (legacyAdmission && !mResourceSystem->openGlHostMemoryBudgetEnabled()))
            ? std::optional<Resource::PreloadAdmission::Reservation>(std::in_place)
            : mResourceSystem->reserveOptionalPreload();
        if (!reservation)
            return; // optional request can retry; never wait on the main thread

        Resource::SpeculativeBudget::Job byteJob;
        if (p1b)
        {
            byteJob = mResourceSystem->speculativeBudget()->tryJob(
                lifetime ? sample : Resource::ResourceSystem::speculativeSample(mResourceSystem), priority);
            if (!byteJob) return;
        }
        while (mPreloadCells.size() >= mMaxCacheSize)
        {
            // throw out oldest cell to make room
            PreloadMap::iterator oldestCell = mPreloadCells.begin();
            double oldestTimestamp = std::numeric_limits<double>::max();
            double threshold = 1.0; // seconds
            for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            {
                if (it->second.mTimeStamp < oldestTimestamp)
                {
                    oldestTimestamp = it->second.mTimeStamp;
                    oldestCell = it;
                }
            }

            if (oldestTimestamp + threshold < timestamp)
            {
                oldestCell->second.mWorkItem->abort();
                if (p1b)
                { oldestCell->second.mRetired = true; retireCompletedPreloads(); return; }
                mPreloadCells.erase(oldestCell);
                ++mEvicted;
            }
            else
                return;
        }

        osg::ref_ptr<PreloadItem> item;
        try
        {
            item = new PreloadItem(&cell, mResourceSystem->getSceneManager(), mBulletShapeManager,
                mResourceSystem->getKeyframeManager(), mTerrain, mLandManager, mPreloadInstances,
                mResourceSystem, mUseLegacyTerrain, std::move(*reservation), std::move(byteJob), priority,
                lifetime && mAnimationTicketCell == &cell && mAnimationTicket && mAnimationTicket->published()
                    && !mAnimationTicket->cancelled() ? mAnimationTicket.get() : nullptr);
        }
        catch (const Resource::SpeculativeDeferred&) { return; }
        mWorkQueue->addWorkItem(item);

        mPreloadCells.emplace(&cell, PreloadEntry(timestamp, item));
        if (mColdRetention) mColdRetention->request(&cell, timestamp, nearFuture);
        if (lifetime && mAnimationTarget == &cell && (!mAnimationTicket || mAnimationTicketCell == &cell))
        {
            retainAnimationProgress(&cell, item->animationProgress());
            mAnimationRetryAt = timestamp + 0.5;
        }
        ++mAdded;
    }

    void CellPreloader::setAnimationTarget(CellStore* cell, double timestamp)
    {
        if (mColdRetention && cell) mColdRetention->near(cell, timestamp);
        if (!Resource::animationDependencyLifetimeEnabled() || !mResourceSystem->speculativeBudget()) return;
        if (mAnimationTarget != cell)
        {
            mAnimationTarget = cell;
            if (mAnimationTargetGeneration != UINT64_MAX) ++mAnimationTargetGeneration;
            mAnimationRetryAt = timestamp;
        }
        mAnimationTargetStamp = timestamp;
        if (mAnimationTicket && (mAnimationTicketCell != cell
            || mAnimationTicket->generation() != mResourceSystem->getVFS()->getIndexGeneration()
            || mAnimationTicket->additionalSources() != static_cast<bool>(Settings::game().mUseAdditionalAnimSources)))
            mAnimationTicket->cancel();
        if (mAnimationTicket && mAnimationTicket->cancelled() && mAnimationTicket->published())
        {
            if (!mResourceSystem->deferRelease(mAnimationTicket, mAnimationTicket->retainedBytes())) return;
            mAnimationTicketCell = nullptr;
        }
        if (!mAnimationTicket && cell)
        {
            const auto found = mPreloadCells.find(cell);
            if (found != mPreloadCells.end() && found->second.mWorkItem)
            {
                auto progress = static_cast<const PreloadItem&>(*found->second.mWorkItem).animationProgress();
                if (progress && !progress->cancelled())
                    retainAnimationProgress(cell, progress);
            }
        }
    }

    void CellPreloader::retainAnimationProgress(CellStore* cell,
        osg::ref_ptr<Resource::AnimationPreparation> progress)
    {
        if (mAnimationTicket == progress) return;
        if (mAnimationTicket)
        {
            // Replacing a target reference must not become its final heavy
            // release on main, even if an old WorkItem was already drained.
            if (!mAnimationTicket->published()
                || !mResourceSystem->deferRelease(mAnimationTicket, mAnimationTicket->retainedBytes())) return;
        }
        mAnimationTicket = std::move(progress);
        mAnimationTicketCell = cell;
    }

    void CellPreloader::notifyLoaded(CellStore* cell)
    {
        if (mColdRetention) mColdRetention->demanded(cell);
        if (mAnimationTarget == cell) setAnimationTarget(nullptr, mAnimationTargetStamp);
        PreloadMap::iterator found = mPreloadCells.find(cell);
        if (found != mPreloadCells.end())
        {
            if (mResourceSystem->speculativeBudget())
            {
                if (!found->second.mDemandUsed) { ++mLoaded; found->second.mDemandUsed = true; }
                found->second.mRetired = true;
                if (found->second.mWorkItem) found->second.mWorkItem->abort();
                retireCompletedPreloads();
                return;
            }
            if (found->second.mWorkItem)
            {
                found->second.mWorkItem->abort();
                found->second.mWorkItem = nullptr;
            }

            mPreloadCells.erase(found);
            ++mLoaded;
        }
    }

    void CellPreloader::clear()
    {
        if (mColdRetention) mColdRetention->clear();
        setAnimationTarget(nullptr, mAnimationTargetStamp);
        if (mResourceSystem->speculativeBudget())
        {
            for (auto& [cell, entry] : mPreloadCells)
            { entry.mRetired = true; if (entry.mWorkItem) entry.mWorkItem->abort(); }
            retireCompletedPreloads();
            return;
        }
        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end();)
        {
            if (it->second.mWorkItem)
            {
                it->second.mWorkItem->abort();
                it->second.mWorkItem = nullptr;
            }

            mPreloadCells.erase(it++);
        }
    }

    void CellPreloader::updateCache(double timestamp)
    {
        const bool lifetime = Resource::animationDependencyLifetimeEnabled() && mResourceSystem->speculativeBudget();
        const auto sample = lifetime ? Resource::ResourceSystem::speculativeSample(mResourceSystem)
                                     : Resource::OpenGlPressureSample{};
        const auto hostPressure = lifetime
            ? sample.decision.state == Resource::OpenGlPressure::Critical ? Resource::HostMemoryPressure::Critical
                : sample.decision.state == Resource::OpenGlPressure::Caution ? Resource::HostMemoryPressure::Trim
                : Resource::HostMemoryPressure::Normal
            : mResourceSystem->hostMemoryPressure();
        if (mColdRetention && mResourceSystem->speculativeBudget())
        {
            const auto retentionSample = lifetime ? sample : Resource::ResourceSystem::speculativeSample(mResourceSystem);
            const auto headroom = mColdHeadroom.update(retentionSample);
            // No pressure is invented from missing data. Caution/Critical keep
            // the existing authoritative broad retirement and cancellation.
            if (headroom.valid && retentionSample.decision.state == Resource::OpenGlPressure::Normal
                && (mColdBaseRetention || (mCoordinateColdHeadroom && headroom.contract)))
            {
                const double ttl = mCoordinateColdHeadroom && headroom.contract ? 5.0 : 30.0;
                mColdRetired += mColdRetention->retire(timestamp, ttl, [&](const CellStore* cell) {
                    auto found = mPreloadCells.find(cell);
                    if (found == mPreloadCells.end() || found->second.mRetired || !found->second.mWorkItem
                        || !found->second.mWorkItem->isDone()) return false;
                    // Completed only: no job wait, no new abort, no main-thread
                    // final release. This policy yields the old 96-owner floor.
                    found->second.mRetired = true;
                    return true;
                }, mAnimationTarget ? std::optional<const CellStore*>(mAnimationTarget) : std::nullopt,
                    mAnimationTicketCell ? std::optional<const CellStore*>(mAnimationTicketCell) : std::nullopt);
            }
        }
        if (lifetime && mAnimationTicket)
        {
            if (mAnimationTargetStamp < timestamp - mExpiryDelay
                || !Resource::animationOwnedWorkMayRemain(sample, mAnimationTicketCell == mAnimationTarget)
                || mAnimationTicket->generation() != mResourceSystem->getVFS()->getIndexGeneration())
                mAnimationTicket->cancel();
            if (mAnimationTicket->cancelled() && mAnimationTicket->published())
            {
                if (mResourceSystem->deferRelease(mAnimationTicket, mAnimationTicket->retainedBytes()))
                    mAnimationTicketCell = nullptr;
            }
        }
        if (mDiagnosticSampler.due())
        {
            const auto admission = mResourceSystem->preloadAdmissionStats();
            std::uint64_t done = 0;
            for (const auto& [cell, entry] : mPreloadCells)
                if (entry.mWorkItem && entry.mWorkItem->isDone()) ++done;
            Debug::RuntimeDiagnostics::recordEvent("preload_cache", "cell_preloader", {}, {
                {"entries", mPreloadCells.size()}, {"completed", done}, {"added", mAdded},
                {"expired", mExpired}, {"evicted", mEvicted}, {"loaded", mLoaded},
                {"minimum", mMinCacheSize}, {"maximum", mMaxCacheSize},
                {"expiry_seconds", static_cast<std::uint64_t>((std::max)(0.0, mExpiryDelay))},
                {"terrain_views", mTerrainViews.size()}, {"terrain_targets", mTerrainPreloadPositions.size()},
                {"terrain_job_pending", mTerrainPreloadItem && !mTerrainPreloadItem->isDone()},
                {"resource_sweep_pending", mUpdateCacheItem && !mUpdateCacheItem->isDone()},
                {"host_pressure", static_cast<std::uint64_t>(hostPressure)},
                {"pressure_released", mPressureReleased}, {"legacy_terrain", mUseLegacyTerrain} });
            // The bounded recorder has 16 fields. Keep admission accounting in
            // its own event instead of silently truncating all five counters.
            Debug::RuntimeDiagnostics::recordEvent("preload_admission", "cell_preloader", {}, {
                {"admission_pending", admission.pending}, {"reservation_estimate_bytes", admission.reservedEstimate},
                {"admission_accepted", admission.admitted}, {"admission_denied", admission.denied},
                {"admission_released", admission.released} });
            if (mColdRetention)
                Debug::RuntimeDiagnostics::recordEvent("cold_preload_retention", "cell_preloader",
                    "Completed owners; payload estimates are not physical RAM", {
                        {"indexed_owners", mColdRetention->size()}, {"retired", mColdRetired},
                        {"base_ttl_seconds", mColdBaseRetention ? 30u : static_cast<unsigned>(mExpiryDelay)},
                        {"shrinking_ttl_seconds", 5},
                        {"headroom_coordination", mCoordinateColdHeadroom},
                        {"short_return_limit", 8}, {"near_future_limit", 32},
                        {"index_available", mColdRetention->available()}});
        }
        // Count live owners rather than map slots: P1B detaches asynchronously,
        // so marking entries must consume the same retention floor as erasing.
        std::size_t retainedCells = mPreloadCells.size();
        if (mResourceSystem->speculativeBudget())
            retainedCells = static_cast<std::size_t>(std::count_if(mPreloadCells.begin(), mPreloadCells.end(),
                [](const auto& pair) { return !pair.second.mRetired; }));
        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end();)
        {
            if ((mResourceSystem->speculativeBudget() ? retainedCells : mPreloadCells.size()) >= mMinCacheSize
                && it->second.mTimeStamp < timestamp - mExpiryDelay)
            {
                if (mResourceSystem->speculativeBudget())
                {
                    if (!it->second.mRetired) { ++mExpired; --retainedCells; }
                    it->second.mRetired = true;
                    if (it->second.mWorkItem) it->second.mWorkItem->abort();
                    ++it;
                }
                else
                {
                    if (it->second.mWorkItem)
                    { it->second.mWorkItem->abort(); it->second.mWorkItem = nullptr; }
                    mPreloadCells.erase(it++); ++mExpired;
                }
            }
            else
                ++it;
        }

        if (mResourceSystem->speculativeBudget())
        {
            for (auto& [cell, entry] : mPreloadCells)
            {
                if (hostPressure != Resource::HostMemoryPressure::Normal)
                {
                    // The actual nearest admitted worker can finish its bounded
                    // progress in Caution. Broad model owners retire when done;
                    // only its ready, charged animation ticket survives demand.
                    const bool finishUrgent = lifetime && cell == mAnimationTarget
                        && hostPressure == Resource::HostMemoryPressure::Trim && entry.mWorkItem
                        && !entry.mWorkItem->isDone() && mAnimationTicketCell == cell
                        && mAnimationTicket && !mAnimationTicket->cancelled();
                    if (!finishUrgent)
                    { entry.mRetired = true; if (entry.mWorkItem) entry.mWorkItem->abort(); }
                }
                if (entry.mWorkItem && entry.mWorkItem->isDone()
                    && !static_cast<const PreloadItem&>(*entry.mWorkItem).fullyPrepared()) entry.mRetired = true;
            }
            retireCompletedPreloads();
            if (lifetime)
            {
                Debug::HitchTrace::Scope policy("animation.retention_policy");
                policy.retain();
                policy.setMetric(0, "sample_generation", sample.generation);
                policy.setMetric(1, "pressure_state", static_cast<std::uint64_t>(sample.decision.state));
                policy.setMetric(2, "target_generation", mAnimationTargetGeneration);
                policy.setMetric(3, "ticket_ready", mAnimationTicket && mAnimationTicket->published());
            }
        }
        const bool v37AdapterPressure = static_cast<bool>(Settings::cells().mV32GpuMemoryManagement)
            && Debug::V3GpuMemory::softPressure();
        const double v37ResourceSweepSeconds = mResourceSystem->speculativeBudget()
            ? ((hostPressure != Resource::HostMemoryPressure::Normal || mResourceSystem->pendingReleases()) ? 0.1 : 1.0)
            : static_cast<bool>(Settings::cells().mV37RelaxedResourceSweep) && !v37AdapterPressure
                && hostPressure == Resource::HostMemoryPressure::Normal
            ? static_cast<double>(Settings::cells().mV37ResourceSweepSeconds)
            : 1.0;
        if (timestamp - mLastResourceCacheUpdate > v37ResourceSweepSeconds
            && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {
            // V3.16 balanced/aggressive modes isolate periodic cache maintenance
            // from the shared preload queue. The dedicated thread is permanently
            // idle-priority and handles no paging-critical work.
            const bool v316IdleSweep = mV316ResourceSweepQueue
                && (static_cast<bool>(Settings::cells().mV316IdleResourceSweep) || mResourceSystem->speculativeBudget());
            std::vector<osg::ref_ptr<SceneUtil::WorkItem>> released;
            // A pressure-aborted preload is not a complete warm cell. Discard
            // it even if pressure has recovered, so a later request can retry.
            // Completed ownership is transferred to the maintenance worker.
            for (auto it = mPreloadCells.begin(); !mResourceSystem->speculativeBudget()
                && it != mPreloadCells.end() && released.size() < 16;)
            {
                const auto& item = it->second.mWorkItem;
                if (item && item->isDone() && !static_cast<const PreloadItem&>(*item).fullyPrepared())
                {
                    released.push_back(std::move(it->second.mWorkItem));
                    it = mPreloadCells.erase(it);
                    ++mPressureReleased;
                }
                else ++it;
            }
            if (!mResourceSystem->speculativeBudget() && hostPressure != Resource::HostMemoryPressure::Normal)
            {
                // Oldest completed preloads first. In-progress jobs get an
                // abort request only, and retain ownership until they finish.
                const std::size_t limit = hostPressure == Resource::HostMemoryPressure::Critical ? 16 : 4;
                released.reserve(limit);
                for (auto& [cell, entry] : mPreloadCells)
                    if (entry.mWorkItem && !entry.mWorkItem->isDone()) entry.mWorkItem->abort();
                const auto releaseTarget = released.size() + limit;
                while (released.size() < releaseTarget)
                {
                    auto oldest = mPreloadCells.end();
                    for (auto it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
                        if (it->second.mWorkItem && it->second.mWorkItem->isDone()
                            && (oldest == mPreloadCells.end()
                                || it->second.mTimeStamp < oldest->second.mTimeStamp)) oldest = it;
                    if (oldest == mPreloadCells.end()) break;
                    released.push_back(std::move(oldest->second.mWorkItem));
                    mPreloadCells.erase(oldest);
                    ++mPressureReleased;
                }
            }
            mUpdateCacheItem = new UpdateCacheItem(mResourceSystem, timestamp, v316IdleSweep, std::move(released));
            if (v316IdleSweep)
                mV316ResourceSweepQueue->addWorkItem(mUpdateCacheItem);
            else
                mWorkQueue->addWorkItem(mUpdateCacheItem, true);
            mLastResourceCacheUpdate = timestamp;
        }

        if (mTerrainPreloadItem && mTerrainPreloadItem->currentFirst())
        {
            auto completed = mTerrainPreloadItem->completedTargets();
            if (!completed.empty())
            {
                mTerrainPreloadItem->traceLoadedPublication(completed.size());
                mLoadedTerrainPositions = std::move(completed);
                mLoadedTerrainTimestamp = timestamp;
            }
        }
        else if (mTerrainPreloadItem && mTerrainPreloadItem->takeReadinessPublication())
        {
            mTerrainPreloadItem->traceLoadedPublication(mTerrainPreloadPositions.size());
            mLoadedTerrainPositions = mTerrainPreloadPositions;
            mLoadedTerrainTimestamp = timestamp;
        }

        if (mTerrainPreloadItem && mTerrainPreloadItem->isDone() && mTerrainPreloadItem->succeeded())
        {
            if (!mTerrainPreloadItem->readinessSplit())
            {
                mTerrainPreloadItem->traceLoadedPublication(mTerrainPreloadPositions.size());
                mLoadedTerrainPositions = mTerrainPreloadPositions;
                mLoadedTerrainTimestamp = timestamp;
            }

            if (static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0)
            {
                ++mV311TerrainTargetCompleted;
                if (!mV311PendingTerrainPreloadPositions.empty())
                {
                    std::vector<PositionCellGrid> pending = std::move(mV311PendingTerrainPreloadPositions);
                    mV311PendingTerrainPreloadPositions.clear();
                    ++mV311TerrainTargetPromoted;
                    setTerrainPreloadPositions(pending);
                }
            }
        }
    }

    void CellPreloader::retireCompletedPreloads()
    {
        // Bounded by the configured cell-owner population; a full retirement
        // queue leaves ownership here and blocks new speculation. Never wait.
        std::size_t moved = 0;
        for (auto it = mPreloadCells.begin(); it != mPreloadCells.end() && moved < 4;)
        {
            auto& entry = it->second;
            if (entry.mRetired && entry.mWorkItem && entry.mWorkItem->isDone())
            {
                auto& item = static_cast<PreloadItem&>(*entry.mWorkItem);
                item.pendingRelease();
                if (!mResourceSystem->deferRelease(entry.mWorkItem, item.retainedEstimate())) break;
                if (mColdRetention) mColdRetention->erase(it->first);
                it = mPreloadCells.erase(it); ++moved; ++mPressureReleased;
            }
            else ++it;
        }
    }

    void CellPreloader::setExpiryDelay(double expiryDelay)
    {
        mExpiryDelay = expiryDelay;
    }

    void CellPreloader::setPreloadInstances(bool preload)
    {
        mPreloadInstances = preload;
    }

    void CellPreloader::setColdRetention(bool enabled, bool coordinateHeadroom)
    {
        mCoordinateColdHeadroom = coordinateHeadroom;
        mColdBaseRetention = enabled;
        mColdRetention = enabled || coordinateHeadroom
            ? std::make_unique<Resource::ColdOwnerRetention<const CellStore*>>() : nullptr;
    }

    void CellPreloader::setWorkQueue(osg::ref_ptr<SceneUtil::WorkQueue> workQueue)
    {
        mWorkQueue = workQueue;
    }

    void CellPreloader::syncTerrainLoad(Loading::Listener& listener, const PositionCellGrid* demanded)
    {
        Debug::HitchTrace::Scope hitchSync("terrain.sync_load", Debug::HitchTrace::CpuTiming::Scoped);
        hitchSync.setLink(mTerrainPreloadItem ? mTerrainPreloadItem->hitchJob() : 0);
        if (mTerrainPreloadItem && mTerrainPreloadItem->currentFirst() && demanded)
        {
            const auto index = mTerrainPreloadItem->matchesWorld(mTerrain)
                ? mTerrainPreloadItem->targetIndex(*demanded) : std::nullopt;
            if (index)
            {
                try { mTerrainPreloadItem->waitTarget(*index, listener); return; }
                catch (const SceneUtil::TerrainTargetUnavailable&) {}
            }
            // A deferred/cancelled future view is never promoted by assertion.
            // Retire its sole mutable owner before preparing the demanded view.
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
            mTerrainPreloadItem = nullptr;
            mTerrainPreloadPositions.clear();
            mV311PendingTerrainPreloadPositions.clear();
            setTerrainPreloadPositions(std::span(demanded, 1));
            mTerrainPreloadItem->waitTarget(0, listener);
            return;
        }
        if (mTerrainPreloadItem != nullptr
            && (mPagingReadinessSplit || mCancellablePagingOptimization || !mTerrainPreloadItem->isDone()))
            mTerrainPreloadItem->wait(listener);
    }

    void CellPreloader::abortTerrainPreloadExcept(const PositionCellGrid* exceptPos)
    {
        Debug::HitchTrace::Scope hitchAbort("terrain.abort_except", Debug::HitchTrace::CpuTiming::Scoped);
        hitchAbort.setLink(mTerrainPreloadItem ? mTerrainPreloadItem->hitchJob() : 0);
        if (exceptPos != nullptr && contains(mTerrainPreloadPositions, *exceptPos, Constants::CellSizeInUnits))
            return;
        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            mTerrainPreloadItem->abort();
            Debug::HitchTrace::Scope jobJoin("terrain.cancel_join", Debug::HitchTrace::CpuTiming::Scoped);
            jobJoin.setLink(mTerrainPreloadItem->hitchJob());
            mTerrainPreloadItem->waitTillDone();
        }
        // An explicitly retired required job is not the owner of the next
        // demanded view. Do not retain its cancelled reporter across reset.
        mTerrainPreloadItem = nullptr;
        setTerrainPreloadPositions({});
    }

    void CellPreloader::setTerrainPreloadPositions(std::span<const PositionCellGrid> positions)
    {
        Debug::HitchTrace::Scope request("terrain.request");
        request.setLink(mTerrainPreloadItem ? mTerrainPreloadItem->hitchJob() : 0);
        request.setMetric(0, "targets", positions.size());
        request.setMetric(1, "legacy_terrain", mUseLegacyTerrain);
        if (!mUseLegacyTerrain) return;
        const bool v311RollingExactActive
            = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0;

        if (positions.empty())
        {
            mTerrainPreloadPositions.clear();
            mLoadedTerrainPositions.clear();
            mV311PendingTerrainPreloadPositions.clear();
        }
        else if (contains(mTerrainPreloadPositions, positions, 128.f)
            && (!mCancellablePagingOptimization || !mTerrainPreloadItem || !mTerrainPreloadItem->isDone()
                || mTerrainPreloadItem->succeeded())
            && (!mTerrainPreloadItem || !mTerrainPreloadItem->currentFirst() || mTerrainPreloadItem->canServe(positions))
            && (!mTerrainPreloadItem || !(Resource::AssetPolicy::mask() & SceneUtil::AssetPaging::Mask)
                || (mTerrainPreloadItem->optionalSourcesMatch() && mTerrainPreloadItem->matchesWorld(mTerrain))))
        {
            // Weak readiness still satisfies this request immediately. A done
            // optional tail can revisit that exact source/target cohort after
            // cooldown; scheduling never sleeps or waits on the gameplay path.
            if (mTerrainPreloadItem
                && mTerrainPreloadItem->optionalRetryDue(SceneUtil::AssetPaging::milliseconds()))
            {
                Debug::HitchTrace::Scope retry("terrain.optional_retry_enqueue");
                retry.retain(); retry.setLink(mTerrainPreloadItem->hitchJob());
                mTerrainPreloadItem = new TerrainPreloadItem(*mTerrainPreloadItem,retry.id());
                ++SceneUtil::AssetPaging::Counters::tailQueued;
                mWorkQueue->addWorkItem(mTerrainPreloadItem);
            }
            return;
        }

        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            if (!v311RollingExactActive || positions.empty())
                return;

            if ((Resource::AssetPolicy::mask() & SceneUtil::AssetPaging::Mask)
                && (!mTerrainPreloadItem->optionalSourcesMatch() || !mTerrainPreloadItem->matchesWorld(mTerrain)))
                mTerrainPreloadItem->abort(); // obsolete optional work; no frame-thread join

            const auto firstBoundsEqual = [](std::span<const PositionCellGrid> a,
                                              std::span<const PositionCellGrid> b) {
                return !a.empty() && !b.empty() && a.front().mCellBounds == b.front().mCellBounds;
            };

            // The running item already targets this exact future grid. Ignore
            // predicted-position jitter until the grid bounds themselves change.
            if (firstBoundsEqual(mTerrainPreloadPositions, positions))
                return;

            // Keep exactly one newest future-grid target. If prediction changes
            // again before the old worker finishes, replace the pending target.
            if (!mV311PendingTerrainPreloadPositions.empty()
                && !firstBoundsEqual(mV311PendingTerrainPreloadPositions, positions))
                ++mV311TerrainTargetReplaced;

            mV311PendingTerrainPreloadPositions.assign(positions.begin(), positions.end());
            request.retain();
            request.setMetric(2, "pending_replacement", 1);
            if (mPagingReadinessSplit && mTerrainPreloadItem->readinessComplete())
                mTerrainPreloadItem->abort();
            return;
        }
        else
        {
            if (mTerrainViews.size() > positions.size())
                mTerrainViews.resize(positions.size());
            else if (mTerrainViews.size() < positions.size())
            {
                for (size_t i = mTerrainViews.size(); i < positions.size(); ++i)
                    mTerrainViews.emplace_back(mTerrain->createView());
            }

            mTerrainPreloadPositions.assign(positions.begin(), positions.end());
            if (!positions.empty())
            {
                request.retain();
                Debug::HitchTrace::Scope create("terrain.job_create");
                create.retain();
                create.setLink(request.id());
                create.setMetric(0, "targets", positions.size());
                if (create.id())
                    for (std::size_t i = 0; i < positions.size(); ++i)
                    {
                        const auto& p = positions[i];
                        const std::string target = "grid=" + std::to_string(p.mCellBounds.x()) + ","
                            + std::to_string(p.mCellBounds.y()) + "," + std::to_string(p.mCellBounds.z()) + ","
                            + std::to_string(p.mCellBounds.w()) + ";position=" + std::to_string(p.mPosition.x())
                            + "," + std::to_string(p.mPosition.y()) + "," + std::to_string(p.mPosition.z());
                        Debug::HitchTrace::Scope targetRow("terrain.target", target,
                            mTerrain->getWorldspace().toString());
                        targetRow.retain();
                        targetRow.setLink(create.id());
                        targetRow.setMetric(0, "target_index", i);
                    }
                mTerrainPreloadItem = new TerrainPreloadItem(mTerrainViews, mTerrain, mResourceSystem,
                    positions, mCancellablePagingOptimization, mPagingReadinessSplit, create.id(), mTerrainCurrentFirst);
                {
                    Debug::HitchTrace::Scope queue("terrain.job_enqueue");
                    queue.retain();
                    queue.setLink(create.id());
                    mWorkQueue->addWorkItem(mTerrainPreloadItem);
                }
            }
        }
    }

    bool CellPreloader::isTerrainLoaded(const PositionCellGrid& position, double referenceTime) const
    {
        return mLoadedTerrainTimestamp + mResourceSystem->getSceneManager()->getExpiryDelay() > referenceTime
            && contains(mLoadedTerrainPositions, position, Constants::CellSizeInUnits);
    }

    void CellPreloader::setTerrain(Terrain::World* terrain)
    {
        if (terrain != mTerrain)
        {
            clearAllTasks();
            mTerrain = terrain;
        }
    }

    void CellPreloader::clearAllTasks()
    {
        if (mTerrainPreloadItem)
        {
            mTerrainPreloadItem->abort();
            {
                Debug::HitchTrace::Scope jobJoin("terrain.clear_join", Debug::HitchTrace::CpuTiming::Scoped);
                jobJoin.setLink(mTerrainPreloadItem->hitchJob());
                mTerrainPreloadItem->waitTillDone();
            }
            mTerrainPreloadItem = nullptr;
        }
        mV311PendingTerrainPreloadPositions.clear();

        if (mUpdateCacheItem)
        {
            mUpdateCacheItem->waitTillDone();
            mUpdateCacheItem = nullptr;
        }

        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            it->second.mWorkItem->abort();

        for (PreloadMap::iterator it = mPreloadCells.begin(); it != mPreloadCells.end(); ++it)
            it->second.mWorkItem->waitTillDone();

        mPreloadCells.clear();
    }

    void CellPreloader::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        stats.setAttribute(frameNumber, "CellPreloader Count", static_cast<double>(mPreloadCells.size()));
        stats.setAttribute(frameNumber, "CellPreloader Added", static_cast<double>(mAdded));
        stats.setAttribute(frameNumber, "CellPreloader Evicted", static_cast<double>(mEvicted));
        stats.setAttribute(frameNumber, "CellPreloader Loaded", static_cast<double>(mLoaded));
        stats.setAttribute(frameNumber, "CellPreloader Expired", static_cast<double>(mExpired));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Completed",
            static_cast<double>(mV311TerrainTargetCompleted));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Replaced",
            static_cast<double>(mV311TerrainTargetReplaced));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Promoted",
            static_cast<double>(mV311TerrainTargetPromoted));
        stats.setAttribute(frameNumber, "V3.11 Terrain Target Pending",
            mV311PendingTerrainPreloadPositions.empty() ? 0.0 : 1.0);
    }
}
