#ifndef OPENMW_COMPONENTS_RESOURCE_RESOURCESYSTEM_H
#define OPENMW_COMPONENTS_RESOURCE_RESOURCESYSTEM_H

#include "hostmemorybudget.hpp"
#include "speculativebudget.hpp"
#include <osg/ref_ptr>
#include <osg/Referenced>
#include "deferredrelease.hpp"

#include <memory>
#include <vector>

namespace VFS
{
    class Manager;
}

namespace osg
{
    class Stats;
    class State;
}

namespace SceneUtil
{
    class WorkQueue;
}

namespace ToUTF8
{
    class StatelessUtf8Encoder;
}

namespace Resource
{

    class SceneManager;
    class ImageManager;
    class BgsmFileManager;
    class NifFileManager;
    class KeyframeManager;
    class BaseResourceManager;
    class AnimBlendRulesManager;
    struct LoadingNifMaintenanceState;

    /// @brief Wrapper class that constructs and provides access to the most commonly used resource subsystems.
    /// @par Resource subsystems can be used with multiple OpenGL contexts, just like the OSG equivalents, but
    ///     are built around the use of a single virtual file system.
    class ResourceSystem
    {
    public:
        explicit ResourceSystem(const VFS::Manager* vfs, double expiryDelay,
            const ToUTF8::StatelessUtf8Encoder* encoder, bool retainNifFiles = false);
        ~ResourceSystem();

        SceneManager* getSceneManager();
        ImageManager* getImageManager();
        BgsmFileManager* getBgsmFileManager();
        NifFileManager* getNifFileManager();
        KeyframeManager* getKeyframeManager();
        AnimBlendRulesManager* getAnimBlendRulesManager();

        // Configure once before workers start. Disabled preserves legacy policy.
        void setHostMemoryBudgetEnabled(bool enabled) { mHostMemoryBudget.setEnabled(enabled); }
        void enableOpenGlHostMemoryBudget(OpenGlPressureConfig config) { mHostMemoryBudget.enableOpenGl(config); }
        void enableOpenGlSpeculativeBudget(OpenGlPressureConfig pressure, SpeculativeBudget::Config config);
        SpeculativeBudget* speculativeBudget() const noexcept { return mSpeculativeBudget.get(); }
        static OpenGlPressureSample speculativeSample(void* self)
        { return static_cast<ResourceSystem*>(self)->mHostMemoryBudget.openGlSample(); }
        template<class T>
        bool deferRelease(osg::ref_ptr<T>& object, std::uint64_t estimate)
        { return mSpeculativeBudget && mDeferredRelease.push(object, estimate); }
        std::size_t pendingReleases() const;
        bool openGlHostMemoryBudgetEnabled() const { return mHostMemoryBudget.openGlEnabled(); }
        HostMemoryPressure hostMemoryPressure() { return mHostMemoryBudget.pressure(); }
        std::optional<PreloadAdmission::Reservation> reserveOptionalPreload()
        { return mHostMemoryBudget.reserveOptionalPreload(); }
        PreloadAdmission::Stats preloadAdmissionStats() const { return mHostMemoryBudget.preloadAdmissionStats(); }

        // Startup-only configuration. LoadingScreen is the UI-owner caller;
        // the borrowed queue is the existing one-thread idle sweep queue.
        void configureLoadingNifMaintenance(bool enabled);
        void attachLoadingNifMaintenanceQueue(SceneUtil::WorkQueue* queue);
        void detachLoadingNifMaintenanceQueue(SceneUtil::WorkQueue* queue = nullptr);
        void setLoadingNifMaintenanceActive(bool active) noexcept;
        void requestLoadingNifMaintenance(double referenceTime) noexcept;

        /// Indicates to each resource manager to clear the cache, i.e. to drop cached objects that are no longer
        /// referenced.
        /// @note May be called from any thread if you do not add or remove resource managers at that point.
        void updateCache(double referenceTime);

        /// Indicates to each resource manager to clear the entire cache.
        /// @note May be called from any thread if you do not add or remove resource managers at that point.
        void clearCache();

        /// Add this ResourceManager to be handled by the ResourceSystem.
        /// @note Does not transfer ownership.
        void addResourceManager(BaseResourceManager* resourceMgr);
        /// @note Do nothing if resourceMgr does not exist.
        /// @note Does not delete resourceMgr.
        void removeResourceManager(BaseResourceManager* resourceMgr);

        /// How long to keep objects in cache after no longer being referenced.
        void setExpiryDelay(double expiryDelay);

        // Startup-only, before any loader/maintenance worker. Zero inherits the
        // ordinary common expiry. Overrides affect unused cache residence only;
        // existing external-owner protection and bounded maintenance still apply.
        void configureVisualCacheRetention(double imageSeconds, double sceneSeconds);

        /// @note May be called from any thread.
        const VFS::Manager* getVFS() const;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const;

        /// Call releaseGLObjects for each resource manager.
        void releaseGLObjects(osg::State* state);

    private:
        // Outlives all managers that borrow it.
        // Monitor must stop before its growth watermark/ledger is destroyed.
        std::unique_ptr<SpeculativeBudget> mSpeculativeBudget;
        HostMemoryBudget mHostMemoryBudget;
        DeferredReleaseQueue mDeferredRelease;
        std::size_t mMaintenanceCursor = 0;
        std::vector<BaseResourceManager*> mMaintenanceOrder;
        void rebuildMaintenanceOrder();
        void updateBudgetedCache(double referenceTime);
        std::unique_ptr<SceneManager> mSceneManager;
        std::unique_ptr<ImageManager> mImageManager;
        std::unique_ptr<BgsmFileManager> mBgsmFileManager;
        std::unique_ptr<NifFileManager> mNifFileManager;
        std::unique_ptr<KeyframeManager> mKeyframeManager;
        std::unique_ptr<AnimBlendRulesManager> mAnimBlendRulesManager;
        // Destroyed/joined before managers and their borrowed charge ledger.
        std::unique_ptr<LoadingNifMaintenanceState> mLoadingNifMaintenance;

        // Store the base classes separately to get convenient access to the common interface
        // Here users can register their own resourcemanager as well
        std::vector<BaseResourceManager*> mResourceManagers;

        const VFS::Manager* mVFS;
        bool mRetainNifFiles = false;
        bool mVisualCacheRetentionConfigured = false;
        double mUnusedImageExpiry = 0.0, mUnusedSceneExpiry = 0.0;

        ResourceSystem(const ResourceSystem&);
        void operator=(const ResourceSystem&);
    };

}

#endif
