#include "cellpreloader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <span>
#include <exception>
#include <stdexcept>

#include <components/sceneutil/pagingwork.hpp>

#include <osg/Stats>

#include <components/debug/debuglog.hpp>
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
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/debug/v3gpumemory.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>
#include <components/vfs/manager.hpp>

#include "../mwrender/landmanager.hpp"

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

            return true;
        }

        std::vector<VFS::Path::NormalizedView>& mOut;
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
            Resource::SpeculativePriority priority = Resource::SpeculativePriority::Background)
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
        {
            if (mUseLegacyTerrain)
                mTerrainView = mTerrain->createView();

            ListModelsVisitor visitor{ mMeshes };
            cell->forEachConst(visitor);
            if (mByteJob)
            {
                Resource::SpeculativeScope scope(mResourceSystem->speculativeBudget(),
                    &Resource::ResourceSystem::speculativeSample, mResourceSystem, mPriority);
                const auto shell = std::uint64_t(mMeshes.capacity()) * sizeof(mMeshes.front()) + sizeof(*this);
                Resource::SpeculativeScope::Stage stage(shell, shell);
                mOwnerCharge = Resource::SpeculativeScope::track(mResourceSystem->speculativeBudget(), {this, 3}, shell, true);
            }
        }

        std::uint64_t retainedEstimate() const noexcept { return mRetainedEstimate.load(std::memory_order_acquire); }
        void pendingRelease() { if (mOwnerCharge) mOwnerCharge->pending(); }
        void abort() override { mAbort = true; }
        bool fullyPrepared() const noexcept { return mFullyPrepared.load(std::memory_order_acquire); }

        /// Preload work to be called from the worker thread.
        void doWork() override
        {
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
            if (mAbort)
                return;
            if (mPriority == Resource::SpeculativePriority::Background
                && mResourceSystem->hostMemoryPressure() != Resource::HostMemoryPressure::Normal)
                return;
            if (mPriority == Resource::SpeculativePriority::NearFuture
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

        osg::ref_ptr<Terrain::View> mTerrainView;

        // keep a ref to the loaded objects to make sure it stays loaded as long as this cell is in the preloaded state
        std::set<osg::ref_ptr<const osg::Object>> mPreloadedObjects;
    };

    class TerrainPreloadItem : public SceneUtil::WorkItem
    {
    public:
        explicit TerrainPreloadItem(const std::vector<osg::ref_ptr<Terrain::View>>& views, Terrain::World* world,
            Resource::ResourceSystem* resourceSystem, std::span<const PositionCellGrid> preloadPositions,
            bool cancellableOptimization = false, bool readinessSplit = false)
            : mAbort(false)
            , mCancellableOptimization(cancellableOptimization)
            , mReadinessSplit(cancellableOptimization && readinessSplit)
            , mTerrainViews(views)
            , mWorld(world)
            , mResourceSystem(resourceSystem)
            , mPreloadPositions(preloadPositions.begin(), preloadPositions.end())
        {
            if (Debug::GameplayDiagnostics::enabled() || Debug::RuntimeDiagnostics::enabled())
                mQueued = Debug::GameplayDiagnostics::Clock::now();
        }

        void doWork() override
        {
            const bool measure = Debug::GameplayDiagnostics::enabled() || Debug::RuntimeDiagnostics::enabled();
            const auto start = measure ? Debug::GameplayDiagnostics::Clock::now()
                                       : Debug::GameplayDiagnostics::Clock::time_point{};
            if (measure) mQueueMs = std::chrono::duration<double, std::milli>(start - mQueued).count();

            const auto prepare = [&](Loading::Reporter& reporter) {
                for (unsigned int i = 0; i < mTerrainViews.size() && i < mPreloadPositions.size() && !mAbort; ++i)
                {
                    SceneUtil::PagingWorkScope::checkpoint();
                    mTerrainViews[i]->reset();
                    mWorld->preload(mTerrainViews[i], mPreloadPositions[i].mPosition, mPreloadPositions[i].mCellBounds,
                        mAbort, reporter);
                }
            };

            if (mCancellableOptimization && mReadinessSplit)
            {
                const auto readyStart = Debug::GameplayDiagnostics::Clock::now();
                try
                {
                    SceneUtil::PagingWorkScope scope(
                        &mAbort, SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
                    prepare(mRequiredReporter);
                    mSucceeded.store(!mAbort.load(), std::memory_order_release);
                }
                catch (const SceneUtil::PagingWorkCancelled&)
                {
                    // Required private views were cancelled before publication.
                }
                catch (...)
                {
                    mRequiredFailure = std::current_exception();
                }
                if (measure)
                    mReadinessMs = std::chrono::duration<double, std::milli>(
                        Debug::GameplayDiagnostics::Clock::now() - readyStart).count();

                // Publish only the required phase to syncTerrainLoad. The work
                // item deliberately remains alive while the stronger V3.13
                // cache upgrade continues on the worker.
                mRequiredReporter.complete();

                if (mSucceeded.load(std::memory_order_acquire) && !mAbort.load(std::memory_order_relaxed))
                {
                    Loading::Reporter optimizationReporter;
                    const auto optionalStart = Debug::GameplayDiagnostics::Clock::now();
                    Resource::SpeculativeBudget* budget = mResourceSystem->speculativeBudget();
                    bool admitted = true;

                    // P2's optional tail is already serialized by the single
                    // TerrainPreloadItem owner, so it does not consume P1B's
                    // reserved near-future JOB slot. It still uses the
                    // NearFuture byte/headroom reservation below. The reserved
                    // job slot remains available to the closest exterior cell.
                    if (!budget && mResourceSystem->openGlHostMemoryBudgetEnabled())
                    {
                        const auto sample = Resource::ResourceSystem::speculativeSample(mResourceSystem);
                        admitted = sample.generation
                            && sample.decision.state != Resource::OpenGlPressure::Critical
                            && sample.decision.state != Resource::OpenGlPressure::Degraded;
                    }
                    else if (mResourceSystem->hostMemoryPressure() == Resource::HostMemoryPressure::Critical)
                        admitted = false;

                    if (admitted)
                    {
                        Resource::SpeculativeScope speculativeScope(budget,
                            &Resource::ResourceSystem::speculativeSample, mResourceSystem,
                            Resource::SpeculativePriority::NearFuture);
                        try
                        {
                            // Bound the weak->strong overlap. Nested resource
                            // stages inherit NearFuture priority; the private
                            // weak view is released after cache replacement.
                            Resource::SpeculativeScope::Stage upgradeStage(
                                128 * Resource::SpeculativeBudget::MiB, 0);
                            SceneUtil::PagingWorkScope scope(
                                &mAbort, SceneUtil::PagingWorkScope::Phase::OptionalOptimization);
                            mOptionalUpgradeRequests.store(mWorld->preloadStrongUpgrade(mTerrainViews.front().get(),
                                mPreloadPositions.front().mPosition, mPreloadPositions.front().mCellBounds,
                                mAbort, optimizationReporter), std::memory_order_release);

                            // Additional target views are independent future
                            // grids; upgrade each without replaying preload().
                            for (std::size_t i = 1; i < mTerrainViews.size()
                                && i < mPreloadPositions.size() && !mAbort; ++i)
                            {
                                mOptionalUpgradeRequests.fetch_add(mWorld->preloadStrongUpgrade(
                                    mTerrainViews[i].get(), mPreloadPositions[i].mPosition,
                                    mPreloadPositions[i].mCellBounds, mAbort, optimizationReporter),
                                    std::memory_order_relaxed);
                            }
                            mOptionalCompleted.store(!mAbort.load(), std::memory_order_release);
                        }
                        catch (const Resource::SpeculativeDeferred&)
                        {
                            mOptionalDeferred.store(true, std::memory_order_release);
                        }
                        catch (const SceneUtil::PagingWorkCancelled&)
                        {
                            // Correct readiness was already published. Obsolete
                            // stronger preparation can disappear without a wait.
                        }
                        catch (const std::exception& e)
                        {
                            Log(Debug::Warning)
                                << "Optional paging optimization failed after required terrain readiness: " << e.what();
                        }
                        catch (...)
                        {
                            Log(Debug::Warning)
                                << "Optional paging optimization failed after required terrain readiness";
                        }
                    }
                    else
                        mOptionalDeferred.store(true, std::memory_order_release);

                    optimizationReporter.complete();
                    if (measure)
                        mOptionalMs = std::chrono::duration<double, std::milli>(
                            Debug::GameplayDiagnostics::Clock::now() - optionalStart).count();
                }
            }
            else if (mCancellableOptimization)
            {
                SceneUtil::PagingWorkScope scope(&mAbort);
                try
                {
                    prepare(mLoadingReporter);
                    mSucceeded.store(!mAbort.load(), std::memory_order_release);
                }
                catch (const SceneUtil::PagingWorkCancelled&)
                {
                    // A cancelled private view is never advertised as ready.
                }
                catch (...)
                {
                    // WorkQueue does not catch exceptions. Preserve failure for
                    // the waiting main thread instead of terminating a worker.
                    mRequiredFailure = std::current_exception();
                }
                mLoadingReporter.complete();
            }
            else
            {
                prepare(mLoadingReporter);
                mLoadingReporter.complete();
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

        void abort() override { mAbort = true; }
        bool succeeded() const noexcept
        { return !mCancellableOptimization || mSucceeded.load(std::memory_order_acquire); }
        bool readinessSplit() const noexcept { return mReadinessSplit; }
        bool readinessComplete() const noexcept
        { return mReadinessSplit && mSucceeded.load(std::memory_order_acquire); }
        bool takeReadinessPublication() noexcept
        {
            return readinessComplete() && !mReadinessPublished.exchange(true, std::memory_order_acq_rel);
        }

        void wait(Loading::Listener& listener)
        {
            Debug::RuntimeDiagnostics::Operation runtimeOperation("required_terrain_wait");
            if (Debug::RuntimeDiagnostics::enabled())
                Debug::RuntimeDiagnostics::recordEvent("wait_dependency", "required_terrain_wait", {},
                    {{"job", reinterpret_cast<std::uintptr_t>(this)}, {"views", mPreloadPositions.size()},
                     {"readiness_split", mReadinessSplit}});
            Debug::GameplayDiagnostics::Operation operation("terrain_wait");

            if (mReadinessSplit)
            {
                mRequiredReporter.wait(listener);
                // Required-phase state is immutable after reporter completion.
                // Do not join the optional strong-upgrade tail.
                if (mRequiredFailure) std::rethrow_exception(mRequiredFailure);
                if (!succeeded())
                    throw std::runtime_error("Required terrain preparation was cancelled; no partial view published");
            }
            else
            {
                mLoadingReporter.wait(listener);
                if (mCancellableOptimization)
                {
                    // Reporter completion precedes WorkQueue::signalDone.
                    waitTillDone();
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
        std::atomic<bool> mAbort;
        const bool mCancellableOptimization;
        const bool mReadinessSplit;
        std::atomic<bool> mSucceeded{false};
        std::atomic<bool> mReadinessPublished{false};
        std::exception_ptr mRequiredFailure;
        std::vector<osg::ref_ptr<Terrain::View>> mTerrainViews;
        Terrain::World* mWorld;
        Resource::ResourceSystem* mResourceSystem;
        std::vector<PositionCellGrid> mPreloadPositions;
        Loading::Reporter mLoadingReporter;
        Loading::Reporter mRequiredReporter;
        Debug::GameplayDiagnostics::Clock::time_point mQueued{};
        std::atomic<double> mQueueMs{0}, mWorkMs{0}, mReadinessMs{0}, mOptionalMs{0};
        std::atomic<std::uint64_t> mOptionalUpgradeRequests{0};
        std::atomic<bool> mOptionalDeferred{false}, mOptionalCompleted{false};
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
            mReleasedPreloads.clear();
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
        , mLastResourceCacheUpdate(0.0)
        , mLoadedTerrainTimestamp(0.0)
    {
        if (static_cast<bool>(Settings::cells().mV316IdleResourceSweep) || mResourceSystem->speculativeBudget())
            mV316ResourceSweepQueue = new SceneUtil::WorkQueue(1);
    }

    CellPreloader::~CellPreloader()
    {
        clearAllTasks();
    }

    void CellPreloader::preload(CellStore& cell, double timestamp, bool nearFuture)
    {
        const bool p1b = mResourceSystem->speculativeBudget() != nullptr;
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
            // A retired or incomplete owner is not a warm ready cell.
            if (found->second.mRetired) return;
            // already preloaded, nothing to do other than updating the timestamp
            found->second.mTimeStamp = timestamp;
            return;
        }

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
                Resource::ResourceSystem::speculativeSample(mResourceSystem), priority);
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
                mResourceSystem, mUseLegacyTerrain, std::move(*reservation), std::move(byteJob), priority);
        }
        catch (const Resource::SpeculativeDeferred&) { return; }
        mWorkQueue->addWorkItem(item);

        mPreloadCells.emplace(&cell, PreloadEntry(timestamp, item));
        ++mAdded;
    }

    void CellPreloader::notifyLoaded(CellStore* cell)
    {
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
        const auto hostPressure = mResourceSystem->hostMemoryPressure();
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
                { entry.mRetired = true; if (entry.mWorkItem) entry.mWorkItem->abort(); }
                if (entry.mWorkItem && entry.mWorkItem->isDone()
                    && !static_cast<const PreloadItem&>(*entry.mWorkItem).fullyPrepared()) entry.mRetired = true;
            }
            retireCompletedPreloads();
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

        if (mTerrainPreloadItem && mTerrainPreloadItem->takeReadinessPublication())
        {
            mLoadedTerrainPositions = mTerrainPreloadPositions;
            mLoadedTerrainTimestamp = timestamp;
        }

        if (mTerrainPreloadItem && mTerrainPreloadItem->isDone() && mTerrainPreloadItem->succeeded())
        {
            if (!mTerrainPreloadItem->readinessSplit())
            {
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

    void CellPreloader::setWorkQueue(osg::ref_ptr<SceneUtil::WorkQueue> workQueue)
    {
        mWorkQueue = workQueue;
    }

    void CellPreloader::syncTerrainLoad(Loading::Listener& listener)
    {
        if (mTerrainPreloadItem != nullptr
            && (mPagingReadinessSplit || mCancellablePagingOptimization || !mTerrainPreloadItem->isDone()))
            mTerrainPreloadItem->wait(listener);
    }

    void CellPreloader::abortTerrainPreloadExcept(const PositionCellGrid* exceptPos)
    {
        if (exceptPos != nullptr && contains(mTerrainPreloadPositions, *exceptPos, Constants::CellSizeInUnits))
            return;
        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            mTerrainPreloadItem->abort();
            mTerrainPreloadItem->waitTillDone();
        }
        setTerrainPreloadPositions({});
    }

    void CellPreloader::setTerrainPreloadPositions(std::span<const PositionCellGrid> positions)
    {
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
                || mTerrainPreloadItem->succeeded()))
            return;

        if (mTerrainPreloadItem && !mTerrainPreloadItem->isDone())
        {
            if (!v311RollingExactActive || positions.empty())
                return;

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
                mTerrainPreloadItem = new TerrainPreloadItem(mTerrainViews, mTerrain, mResourceSystem,
                    positions, mCancellablePagingOptimization, mPagingReadinessSplit);
                mWorkQueue->addWorkItem(mTerrainPreloadItem);
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
            mTerrainPreloadItem->waitTillDone();
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
