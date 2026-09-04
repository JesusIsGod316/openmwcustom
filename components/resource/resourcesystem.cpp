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
            (*it)->updateCache(referenceTime);
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
