#ifndef OPENMW_MWWORLD_CELLPRELOADER_H
#define OPENMW_MWWORLD_CELLPRELOADER_H

#include "positioncellgrid.hpp"
#include <components/debug/runtimediagnostics.hpp>

#include <components/sceneutil/workqueue.hpp>
#include <components/resource/ramretention.hpp>

#include <osg/ref_ptr>

#include <map>
#include <span>
#include <memory>

namespace osg
{
    class Stats;
}

namespace Resource
{
    class ResourceSystem;
    class BulletShapeManager;
    class AnimationPreparation;
}

namespace Terrain
{
    class World;
    class View;
}

namespace MWRender
{
    class LandManager;
}

namespace Loading
{
    class Listener;
}

namespace MWWorld
{
    class CellStore;
    class TerrainPreloadItem;

    class CellPreloader
    {
    public:
        CellPreloader(Resource::ResourceSystem* resourceSystem, Resource::BulletShapeManager* bulletShapeManager,
            Terrain::World* terrain, MWRender::LandManager* landManager, bool useLegacyTerrain = true);
        ~CellPreloader();

        /// Ask a background thread to preload rendering meshes and collision shapes for objects in this cell.
        /// @note The cell itself must be in State_Loaded or State_Preloaded.
        void preload(MWWorld::CellStore& cell, double timestamp, bool nearFuture = false);
        // Main-thread nearest-target prediction; no dependency scan or worker wait.
        void setAnimationTarget(MWWorld::CellStore* cell, double timestamp);

        void notifyLoaded(MWWorld::CellStore* cell);

        void clear();

        /// Removes preloaded cells that have not had a preload request for a while.
        void updateCache(double timestamp);

        /// How long to keep a preloaded cell in cache after it's no longer requested.
        void setExpiryDelay(double expiryDelay);

        /// The minimum number of preloaded cells before unused cells get thrown out.
        void setMinCacheSize(std::size_t value) { mMinCacheSize = value; }

        /// The maximum number of preloaded cells.
        void setMaxCacheSize(std::size_t value) { mMaxCacheSize = value; }

        /// Enables the creation of instances in the preloading thread.
        void setPreloadInstances(bool preload);
        // Startup-only independent C1/C4. Existing pressure retirement wins.
        void setColdRetention(bool enabled, bool coordinateHeadroom = false);

        std::size_t getMaxCacheSize() const { return mMaxCacheSize; }

        std::size_t getCacheSize() const { return mPreloadCells.size(); }

        bool isPreloaded(const CellStore& cell) const { const auto it = mPreloadCells.find(&cell); return it != mPreloadCells.end() && !it->second.mRetired; }

        void setWorkQueue(osg::ref_ptr<SceneUtil::WorkQueue> workQueue);

        bool usesLegacyTerrain() const noexcept { return mUseLegacyTerrain; }
        void setTerrainPreloadPositions(std::span<const PositionCellGrid> positions);

        void syncTerrainLoad(Loading::Listener& listener, const PositionCellGrid* demanded = nullptr);
        bool terrainCurrentFirstEnabled() const { return mTerrainCurrentFirst; }
        void abortTerrainPreloadExcept(const PositionCellGrid* exceptPos);
        bool isTerrainLoaded(const PositionCellGrid& position, double referenceTime) const;
        void setTerrain(Terrain::World* terrain);

        void reportStats(unsigned int frameNumber, osg::Stats& stats) const;

    private:
        void clearAllTasks();
        void retireCompletedPreloads();
        void retainAnimationProgress(CellStore* cell, osg::ref_ptr<Resource::AnimationPreparation> progress);
        Debug::RuntimeDiagnostics::Sampler mDiagnosticSampler;

        Resource::ResourceSystem* mResourceSystem;
        Resource::BulletShapeManager* mBulletShapeManager;
        Terrain::World* mTerrain;
        MWRender::LandManager* mLandManager;
        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<SceneUtil::WorkQueue> mV316ResourceSweepQueue;
        double mExpiryDelay;
        std::size_t mMinCacheSize = 0;
        std::size_t mMaxCacheSize = 0;
        bool mPreloadInstances;
        const bool mUseLegacyTerrain;
        const bool mCancellablePagingOptimization;
        const bool mPagingReadinessSplit;
        const bool mTerrainCurrentFirst;

        double mLastResourceCacheUpdate;

        struct PreloadEntry
        {
            PreloadEntry(double timestamp, osg::ref_ptr<SceneUtil::WorkItem> workItem)
                : mTimeStamp(timestamp)
                , mWorkItem(std::move(workItem))
            {
            }
            PreloadEntry()
                : mTimeStamp(0.0)
            {
            }

            double mTimeStamp;
            osg::ref_ptr<SceneUtil::WorkItem> mWorkItem;
            bool mRetired = false;
            bool mDemandUsed = false;
        };
        typedef std::map<const MWWorld::CellStore*, PreloadEntry> PreloadMap;

        // Cells that are currently being preloaded, or have already finished preloading
        PreloadMap mPreloadCells;
        MWWorld::CellStore* mAnimationTarget = nullptr;
        MWWorld::CellStore* mAnimationTicketCell = nullptr;
        osg::ref_ptr<Resource::AnimationPreparation> mAnimationTicket;
        double mAnimationTargetStamp = 0.0;
        double mAnimationRetryAt = 0.0;
        std::uint64_t mAnimationTargetGeneration = 0;

        std::vector<osg::ref_ptr<Terrain::View>> mTerrainViews;
        std::vector<PositionCellGrid> mTerrainPreloadPositions;
        std::vector<PositionCellGrid> mV311PendingTerrainPreloadPositions;
        osg::ref_ptr<TerrainPreloadItem> mTerrainPreloadItem;
        osg::ref_ptr<SceneUtil::WorkItem> mUpdateCacheItem;

        std::vector<PositionCellGrid> mLoadedTerrainPositions;
        double mLoadedTerrainTimestamp;
        std::size_t mEvicted = 0;
        std::size_t mAdded = 0;
        std::size_t mExpired = 0;
        std::size_t mLoaded = 0;
        std::size_t mPressureReleased = 0;
        std::unique_ptr<Resource::ColdOwnerRetention<const CellStore*>> mColdRetention;
        Resource::RamHeadroomPolicy mColdHeadroom;
        bool mCoordinateColdHeadroom = false;
        bool mColdBaseRetention = false;
        std::size_t mColdRetired = 0;

        std::size_t mV311TerrainTargetCompleted = 0;
        std::size_t mV311TerrainTargetReplaced = 0;
        std::size_t mV311TerrainTargetPromoted = 0;
    };

}

#endif
