#include "resourcesystem.hpp"

#include <algorithm>
#include "cachemaintenance.hpp"

#include <components/debug/v3diagnostics.hpp>

#include "animblendrulesmanager.hpp"
#include "bgsmfilemanager.hpp"
#include "imagemanager.hpp"
#include "keyframemanager.hpp"
#include "niffilemanager.hpp"
#include "scenemanager.hpp"

namespace Resource
{

    ResourceSystem::ResourceSystem(const VFS::Manager* vfs, double expiryDelay,
        const ToUTF8::StatelessUtf8Encoder* encoder, bool retainNifFiles)
        : mVFS(vfs)
        , mRetainNifFiles(retainNifFiles)
    {
        mNifFileManager = std::make_unique<NifFileManager>(vfs, encoder);
        if (mRetainNifFiles)
            mNifFileManager->setExpiryDelay(expiryDelay);
        mBgsmFileManager = std::make_unique<BgsmFileManager>(vfs, expiryDelay);
        mImageManager = std::make_unique<ImageManager>(vfs, expiryDelay);
        mSceneManager = std::make_unique<SceneManager>(
            vfs, mImageManager.get(), mNifFileManager.get(), mBgsmFileManager.get(), expiryDelay);
        mSceneManager->setHostMemoryBudget(&mHostMemoryBudget);
        mKeyframeManager = std::make_unique<KeyframeManager>(vfs, mSceneManager.get(), expiryDelay, encoder);
        mAnimBlendRulesManager = std::make_unique<AnimBlendRulesManager>(vfs, expiryDelay);

        addResourceManager(mNifFileManager.get());
        addResourceManager(mBgsmFileManager.get());
        addResourceManager(mKeyframeManager.get());
        // note, scene references images so add images afterwards for correct implementation of updateCache()
        addResourceManager(mSceneManager.get());
        addResourceManager(mImageManager.get());
        addResourceManager(mAnimBlendRulesManager.get());
    }

    ResourceSystem::~ResourceSystem()
    {
        // this has to be defined in the .cpp file as we can't delete incomplete types

        // Engine teardown has already joined preload/maintenance workers.
        mDeferredRelease.clear();
        mResourceManagers.clear();
    }

    SceneManager* ResourceSystem::getSceneManager()
    {
        return mSceneManager.get();
    }

    ImageManager* ResourceSystem::getImageManager()
    {
        return mImageManager.get();
    }

    BgsmFileManager* ResourceSystem::getBgsmFileManager()
    {
        return mBgsmFileManager.get();
    }

    NifFileManager* ResourceSystem::getNifFileManager()
    {
        return mNifFileManager.get();
    }

    KeyframeManager* ResourceSystem::getKeyframeManager()
    {
        return mKeyframeManager.get();
    }

    AnimBlendRulesManager* ResourceSystem::getAnimBlendRulesManager()
    {
        return mAnimBlendRulesManager.get();
    }

    void ResourceSystem::setExpiryDelay(double expiryDelay)
    {
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->setExpiryDelay(expiryDelay);

        // Upstream drops parsed NIFs immediately. Overdrive intentionally retains them so paging and
        // collision paths can reuse already-parsed source data during long sessions.
        mNifFileManager->setExpiryDelay(mRetainNifFiles ? expiryDelay : 0.0);
    }

    void ResourceSystem::updateCache(double referenceTime)
    {
        if (mSpeculativeBudget) { updateBudgetedCache(referenceTime); return; }
        Debug::V3Diagnostics::TraceScope trace("resource", "resource_cache_update", "all_managers", 0.5);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::resourceWriter(), "resource_cache_update", "all_managers", 0.5);
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
        {
            (*it)->updateCache(referenceTime);
            (*it)->reportRuntimeDiagnostics(referenceTime);
        }
        const auto pressure = mHostMemoryBudget.pressure();
        if (pressure != HostMemoryPressure::Normal)
        {
            const std::size_t maximum = pressure == HostMemoryPressure::Critical ? 512 : 128;
            std::size_t removed = 0;
            // Retaining owners (paging, collision, etc.) first, then templates
            // and shared state, and finally images. Never clear a live graph.
            for (BaseResourceManager* manager : mResourceManagers)
                if (manager != mSceneManager.get() && manager != mImageManager.get())
                    removed += manager->trimCache(maximum);
            removed += mSceneManager->trimCache(maximum);
            removed += mImageManager->trimCache(maximum);
            if (Debug::RuntimeDiagnostics::enabled())
            {
                const auto memory = mHostMemoryBudget.snapshot();
                auto limits = HostMemoryPolicy::limits(memory.physicalTotal);
                if (mHostMemoryBudget.openGlEnabled())
                {
                    const auto gl = mHostMemoryBudget.openGlSample();
                    limits.reserve = gl.decision.limits.physicalReserve;
                    limits.privateSoft = 0; // no automatic private/RAM cap in GL-P1A
                }
                Debug::RuntimeDiagnostics::recordEvent("host_memory_trim", "optional_resource_owners", {}, {
                    {"pressure", static_cast<std::uint64_t>(pressure)}, {"removed_entries", removed},
                    {"per_owner_limit", maximum}, {"physical_valid", memory.physicalValid},
                    {"physical_available_bytes", memory.physicalAvailable},
                    {"private_commit_bytes", memory.privateCommit}, {"process_valid", memory.processValid},
                    {"commit_available_bytes", memory.commitAvailable}, {"commit_valid", memory.commitValid},
                    {"reserve_bytes", limits.reserve}, {"private_soft_bytes", limits.privateSoft}});
            }
        }
    }

    void ResourceSystem::clearCache()
    {
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->clearCache();
    }

    void ResourceSystem::addResourceManager(BaseResourceManager* resourceMgr)
    {
        mResourceManagers.push_back(resourceMgr);
        if (mSpeculativeBudget)
        { resourceMgr->setSpeculativeBudget(mSpeculativeBudget.get()); rebuildMaintenanceOrder(); }
    }

    void ResourceSystem::removeResourceManager(BaseResourceManager* resourceMgr)
    {
        std::vector<BaseResourceManager*>::iterator found
            = std::find(mResourceManagers.begin(), mResourceManagers.end(), resourceMgr);
        if (found != mResourceManagers.end())
        { mResourceManagers.erase(found); if (mSpeculativeBudget) rebuildMaintenanceOrder(); }
    }


    void ResourceSystem::enableOpenGlSpeculativeBudget(OpenGlPressureConfig pressure, SpeculativeBudget::Config config)
    {
        mSpeculativeBudget = std::make_unique<SpeculativeBudget>(config);
        mHostMemoryBudget.enableOpenGl(pressure, mSpeculativeBudget->watermark());
        for (auto* manager : mResourceManagers) manager->setSpeculativeBudget(mSpeculativeBudget.get());
        rebuildMaintenanceOrder();
    }

    void ResourceSystem::rebuildMaintenanceOrder()
    {
        mMaintenanceOrder.clear();
        for (auto* manager : mResourceManagers)
            if (manager != mSceneManager.get() && manager != mImageManager.get()) mMaintenanceOrder.push_back(manager);
        mMaintenanceOrder.push_back(mSceneManager.get());
        mMaintenanceOrder.push_back(mImageManager.get());
        mMaintenanceCursor = 0;
    }

    std::size_t ResourceSystem::pendingReleases() const
    { return mDeferredRelease.stats().owners; }

    void ResourceSystem::updateBudgetedCache(double referenceTime)
    {
        CacheMaintenanceBudget budget;
        CacheMaintenanceScope scope(budget);
        // This executes on the existing dedicated resource worker. Never wait
        // for a running preload and never perform GL finish/wait here.
        mDeferredRelease.drain(budget);
        const auto pressure = mHostMemoryBudget.pressure();
        // Rotating start avoids starvation when a manager or a destructor uses
        // the remaining time. Within a cycle owners precede templates/images.
        for (std::size_t visited = 0; visited < mMaintenanceOrder.size() && budget.available(); ++visited)
        {
            auto* manager = mMaintenanceOrder[mMaintenanceCursor];
            mMaintenanceCursor = (mMaintenanceCursor + 1) % mMaintenanceOrder.size();
            manager->updateCache(referenceTime);
            if (pressure != HostMemoryPressure::Normal && budget.available()) manager->trimCache(8);
        }
        if (Debug::RuntimeDiagnostics::enabled())
        {
            const auto stats = mSpeculativeBudget->stats();
            Debug::RuntimeDiagnostics::recordEvent("opimizedmw_memory", "p1b", "Owner charges; not total process/VRAM bytes", {
                {"future_bytes", stats.futureBytes}, {"known_owner_bytes", stats.knownOwnerBytes},
                {"estimated_owner_bytes", stats.estimatedOwnerBytes}, {"unknown_owners", stats.unknownOwners},
                {"pending_owner_bytes", stats.pendingOwnerBytes}, {"jobs", stats.jobs}, {"denied", stats.denied},
                {"duplicate_defers", stats.duplicateRequests}, {"shared_claims", stats.sharedClaims},
                {"demand_hits", stats.demandHits}, {"prefetch_hits", stats.prefetchHits},
                {"scanned", budget.scanned}, {"released", budget.released}, {"pending_owners", pendingReleases()} });
        }
    }

    const VFS::Manager* ResourceSystem::getVFS() const
    {
        return mVFS;
    }

    void ResourceSystem::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->reportStats(frameNumber, stats);
    }

    void ResourceSystem::releaseGLObjects(osg::State* state)
    {
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->releaseGLObjects(state);
    }

}
