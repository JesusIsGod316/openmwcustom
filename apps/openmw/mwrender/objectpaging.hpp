#ifndef OPENMW_MWRENDER_OBJECTPAGING_H
#define OPENMW_MWRENDER_OBJECTPAGING_H

#include <components/esm3/refnum.hpp>
#include <components/resource/resourcemanager.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/sceneutil/assetpaging.hpp>

#include <osg/ref_ptr>
#include <osg/observer_ptr>
#include <osg/Camera>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <utility>
#include <functional>

namespace Resource
{
    class SceneManager;
}
namespace osg { class Camera; }

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace OcclusionCulling
{
    class OcclusionStorage;
}

namespace MWRender
{

    typedef std::tuple<osg::Vec2f, float, bool> ChunkId; // Center, Size, ActiveGrid

    class ObjectPaging : public Resource::GenericResourceManager<ChunkId>, public Terrain::QuadTreeWorld::ChunkManager
    {
    public:
        ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace);
        ~ObjectPaging() = default;

        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        osg::ref_ptr<osg::Node> createChunk(float size, const osg::Vec2f& center, bool activeGrid,
            const osg::Vec3f& viewPoint, bool compile, unsigned char lod);

        unsigned int getNodeMask() override;
        bool supportsStrongPagingUpgrade() const override { return true; }
        bool supportsDistantStrongPagingUpgrade() const override;

        /// @return true if view needs rebuild
        bool enableObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled);

        /// @return true if view needs rebuild
        bool blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell);

        void clear();
        void clearCache() override;

        /// Must be called after clear() before rendering starts.
        /// @return true if view needs rebuild
        bool unlockCache();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;
        void setAssetMainCamera(osg::Camera* camera) { mAssetMainCamera = camera; }
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
        // Isolated fixture substitutes only parsed static reference population.
        // Compile every TU that sees this class with the same target-wide macro;
        // never link this definition to the normal openmw-lib definition.
        struct FixtureReference
        {
            osg::ref_ptr<const osg::Node> original;
            VFS::Path::Normalized model;
            int type = 0;
            osg::Vec3f position{};
            float scale = 1.f;
            bool occluderOwner = false;
        };
        void setFixturePopulation(std::vector<FixtureReference> population) { mFixturePopulation = std::move(population); }
        void setFixtureBeforeCache(std::function<void()> checkpoint) { mFixtureBeforeCache = std::move(checkpoint); }
        void setFixtureBeforePublish(std::function<void()> checkpoint) { mFixtureBeforePublish = std::move(checkpoint); }
        void setFixtureBeforePrivate(std::function<void()> checkpoint) { mFixtureBeforePrivate = std::move(checkpoint); }
        // Runs on the unpublished exact post-optimizer control, outside cache
        // and shader-producer locks. Fixture-only ordinary GL warming seam.
        void setFixtureBeforeLodCook(std::function<void(osg::Node&)> checkpoint) { mFixtureBeforeLodCook = std::move(checkpoint); }
#endif

        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);

        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
            OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion = false);

        // V3.10: true only while Scene is synchronously waiting for the one-time
        // initial multi-view frontload. Worker ObjectPaging construction reads this
        // flag, so it must be atomic. Later predictive/background preload remains
        // deliberately outside this gate.
        void setV310InitialFrontloadActive(bool active)
        {
            mV310InitialFrontloadActive.store(active, std::memory_order_release);
        }

    private:
        Resource::SceneManager* mSceneManager;
        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;
        unsigned int mMaxTriangles = 30000;
        OcclusionCulling::OcclusionStorage* mOcclusionStorage = nullptr;
        bool mV35CoarseChunkOcclusion = false;
        bool mActiveGrid;
        bool mDebugBatches;
        float mMergeFactor;
        float mMinSize;
        float mMinSizeMergeFactor;
        float mMinSizeCostMultiplier;
        osg::observer_ptr<osg::Camera> mAssetMainCamera;
        std::atomic_uint64_t mAssetGeneration{ 1 };

        std::atomic_bool mV310InitialFrontloadActive{ false };

        mutable std::mutex mV311PreparedActiveMutex;
        std::set<ChunkId> mV311PreparedActiveChunks;
        std::atomic_uint64_t mV311PreparedActiveBuilt{ 0 };
        std::atomic_uint64_t mV311PreparedActiveHits{ 0 };
        std::atomic_uint64_t mV311DemandFallbacks{ 0 };

        struct V313ChunkQuality
        {
            unsigned char mPrepareMode = 0;
            unsigned char mSpatialMode = 0;
            unsigned char mP3OptionalMask = 0;
            std::uint64_t mAssetVfsGeneration = 0;
            std::uint64_t mAssetShaderGeneration = 0;
            SceneUtil::AssetPaging::Quality mAssets;
        };
        mutable std::mutex mV313ChunkQualityMutex;
        std::map<ChunkId, V313ChunkQuality> mV313ChunkQualities;
        std::set<ChunkId> mV313StrongUpgradeInFlight;
        std::atomic_uint64_t mV313WeakCacheHitOnStrongPrepare{ 0 };
        std::atomic_uint64_t mV313UpgradeBuilt{ 0 };
        std::atomic_uint64_t mV313UpgradeInstalled{ 0 };
        std::atomic_uint64_t mV313UpgradeCoalesced{ 0 };

        unsigned char p3OptionalDistantMask() const;
        osg::ref_ptr<osg::Node> createChunkImpl(float size, const osg::Vec2f& center, bool activeGrid,
            const osg::Vec3f& viewPoint, bool compile, unsigned char lod, SceneUtil::AssetPaging::Outcome& outcome,
            bool prepareActiveLod = false);
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
        std::vector<FixtureReference> mFixturePopulation;
        std::function<void()> mFixtureBeforeCache;
        std::function<void()> mFixtureBeforePublish;
        std::function<void()> mFixtureBeforePrivate;
        std::function<void(osg::Node&)> mFixtureBeforeLodCook;
#endif

        std::mutex mRefTrackerMutex;
        struct RefTracker
        {
            std::set<ESM::RefNum> mDisabled;
            std::set<ESM::RefNum> mBlacklist;
            bool operator==(const RefTracker& other) const
            {
                return mDisabled == other.mDisabled && mBlacklist == other.mBlacklist;
            }
        };
        RefTracker mRefTracker;
        RefTracker mRefTrackerNew;
        bool mRefTrackerLocked;

        const RefTracker& getRefTracker() const { return mRefTracker; }
        RefTracker& getWritableRefTracker() { return mRefTrackerLocked ? mRefTrackerNew : mRefTracker; }

        std::mutex mSizeCacheMutex;
        typedef std::map<ESM::RefNum, float> SizeCache;
        SizeCache mSizeCache;

        std::mutex mLODNameCacheMutex;
        typedef std::pair<std::string, unsigned char> LODNameCacheKey; // Key: mesh name, lod level
        using LODNameCache = std::map<LODNameCacheKey, VFS::Path::Normalized>; // Cache: key, mesh name to use
        LODNameCache mLODNameCache;
    };

    class RefnumMarker : public osg::Object
    {
    public:
        RefnumMarker()
            : mNumVertices(0)
        {
        }
        RefnumMarker(const RefnumMarker& copy, osg::CopyOp co)
            : mRefnum(copy.mRefnum)
            , mNumVertices(copy.mNumVertices)
        {
        }
        META_Object(MWRender, RefnumMarker)

        ESM::RefNum mRefnum;
        unsigned int mNumVertices;
    };
}

#endif
