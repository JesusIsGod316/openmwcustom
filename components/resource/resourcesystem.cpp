#include "resourcesystem.hpp"

#include <algorithm>

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
                const auto limits = HostMemoryPolicy::limits(memory.physicalTotal);
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
    }

    void ResourceSystem::removeResourceManager(BaseResourceManager* resourceMgr)
    {
        std::vector<BaseResourceManager*>::iterator found
            = std::find(mResourceManagers.begin(), mResourceManagers.end(), resourceMgr);
        if (found != mResourceManagers.end())
            mResourceManagers.erase(found);
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
