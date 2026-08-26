import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.5 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.5 patched {rel} ({count} match(es))")


# Settings: all V3.5 behavior is default-off.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<bool> mV34BroadenOcclusion{ mIndex, "Camera", "v3.4 broaden occlusion" };''',
    '''        SettingValue<bool> mV34BroadenOcclusion{ mIndex, "Camera", "v3.4 broaden occlusion" };
        SettingValue<bool> mV35CoarseChunkOcclusion{ mIndex, "Camera", "v3.5 coarse chunk occlusion" };''',
)

replace_exact(
    "components/settings/categories/shadows.hpp",
    '''        SettingValue<int> mV33FarCascadeResolutionDivisor{ mIndex, "Shadows",
            "v3.3 far cascade resolution divisor", makeClampSanitizerInt(1, 4) };''',
    '''        SettingValue<int> mV33FarCascadeResolutionDivisor{ mIndex, "Shadows",
            "v3.3 far cascade resolution divisor", makeClampSanitizerInt(1, 4) };
        SettingValue<bool> mV35AllowDynamicFarCascadeReuse{ mIndex, "Shadows",
            "v3.5 allow dynamic far cascade reuse" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.4 broaden occlusion = false

[Cells]''',
    '''v3.4 broaden occlusion = false

# V3.5 coarse MSOC. Culls whole paged-object and groundcover chunks with tight chunk bounds.
# Unlike V3.4 broadened MSOC, this does not lower the individual-occluder size threshold or raise its triangle budget.
v3.5 coarse chunk occlusion = false

[Cells]''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

# V3.3/V3.4 far-cascade-only GPU experiment.''',
    '''v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

# V3.5 permits the existing bounded far-cascade reuse path when actor/player shadows are enabled.
# When false, V3.3 behavior is preserved and actor/player shadows force interval 1.
v3.5 allow dynamic far cascade reuse = false

# V3.3/V3.4 far-cascade-only GPU experiment.''',
)

# Occlusion component: category-aware counters for coarse whole-chunk rejection.
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''namespace SceneUtil
{
    /// Wraps Intel's Masked Software Occlusion Culling library.''',
    '''namespace SceneUtil
{
    enum class OcclusionTestCategory
    {
        PagedChunk,
        GroundcoverChunk,
    };

    /// Wraps Intel's Masked Software Occlusion Culling library.''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        bool isActive() const''',
    '''        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        /// Test a coarse rendering group against the full buffer while retaining category telemetry.
        bool testVisibleCoarseAABB(const osg::BoundingBox& worldBB, OcclusionTestCategory category) const;

        bool isActive() const''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                "frame,epoch_ms,clear_ms,terrain_build_ms,terrain_raster_ms,building_raster_ms,"
                "aabb_total_ms,testrect_ms,test_calls,building_occluders,building_tris,aabbs_tested,aabbs_occluded");''',
    '''                "frame,epoch_ms,clear_ms,terrain_build_ms,terrain_raster_ms,building_raster_ms,"
                "aabb_total_ms,testrect_ms,test_calls,building_occluders,building_tris,aabbs_tested,aabbs_occluded,"
                "paged_chunks_tested,paged_chunks_occluded,groundcover_chunks_tested,groundcover_chunks_occluded");''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                << mBuildingRasterMs << ',' << mAabbTotalMs << ',' << mTestRectMs << ',' << mDetailedTestCalls << ','
                << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumTested << ',' << mNumOccluded;''',
    '''                << mBuildingRasterMs << ',' << mAabbTotalMs << ',' << mTestRectMs << ',' << mDetailedTestCalls << ','
                << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumTested << ',' << mNumOccluded << ','
                << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                << mGroundcoverChunksOccluded;''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                stream << "occlusion_frame,cull_traverse_ms,building_occluders,building_tris,building_verts,"
                          "aabbs_tested,aabbs_occluded,rejection_pct,cache_mem_hits_total,cache_db_hits_total,"
                          "cache_misses_total,cache_writes_total\\n";''',
    '''                stream << "occlusion_frame,cull_traverse_ms,building_occluders,building_tris,building_verts,"
                          "aabbs_tested,aabbs_occluded,rejection_pct,paged_chunks_tested,paged_chunks_occluded,"
                          "groundcover_chunks_tested,groundcover_chunks_occluded,cache_mem_hits_total,cache_db_hits_total,"
                          "cache_misses_total,cache_writes_total\\n";''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                   << mNumTested << ',' << mNumOccluded << ',' << rejectionPct << ','
                   << cache.memHits << ',' << cache.dbHits << ',' << cache.misses << ',' << cache.writes << '\\n';''',
    '''                   << mNumTested << ',' << mNumOccluded << ',' << rejectionPct << ','
                   << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                   << mGroundcoverChunksOccluded << ',' << cache.memHits << ',' << cache.dbHits << ',' << cache.misses
                   << ',' << cache.writes << '\\n';''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        unsigned int mNumBuildingVerts = 0;

        // V3 telemetry is opt-in.''',
    '''        unsigned int mNumBuildingVerts = 0;
        mutable unsigned int mPagedChunksTested = 0;
        mutable unsigned int mPagedChunksOccluded = 0;
        mutable unsigned int mGroundcoverChunksTested = 0;
        mutable unsigned int mGroundcoverChunksOccluded = 0;

        // V3 telemetry is opt-in.''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''        mDetailedTestCalls = 0;
        if (!mMOC)''',
    '''        mDetailedTestCalls = 0;
        mPagedChunksTested = 0;
        mPagedChunksOccluded = 0;
        mGroundcoverChunksTested = 0;
        mGroundcoverChunksOccluded = 0;
        if (!mMOC)''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''    bool OcclusionCuller::testVisibleAABB(const osg::BoundingBox& worldBB) const
    {
        if (!mFrameActive)
            return true;

        ++mNumTested;
        const bool visible = testVisibleAABBImpl(mMOC, worldBB);
        if (!visible)
            ++mNumOccluded;
        return visible;
    }

    void OcclusionCuller::computePixelDepthBuffer''',
    '''    bool OcclusionCuller::testVisibleAABB(const osg::BoundingBox& worldBB) const
    {
        if (!mFrameActive)
            return true;

        ++mNumTested;
        const bool visible = testVisibleAABBImpl(mMOC, worldBB);
        if (!visible)
            ++mNumOccluded;
        return visible;
    }

    bool OcclusionCuller::testVisibleCoarseAABB(
        const osg::BoundingBox& worldBB, OcclusionTestCategory category) const
    {
        if (!mFrameActive)
            return true;

        ++mNumTested;
        unsigned int* tested = nullptr;
        unsigned int* occluded = nullptr;
        if (category == OcclusionTestCategory::PagedChunk)
        {
            tested = &mPagedChunksTested;
            occluded = &mPagedChunksOccluded;
        }
        else
        {
            tested = &mGroundcoverChunksTested;
            occluded = &mGroundcoverChunksOccluded;
        }
        ++*tested;

        const bool visible = testVisibleAABBImpl(mMOC, worldBB);
        if (!visible)
        {
            ++mNumOccluded;
            ++*occluded;
        }
        return visible;
    }

    void OcclusionCuller::computePixelDepthBuffer''',
)

# Renderer-side coarse callbacks.
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        PagedOccluderData(const PagedOccluderData& copy, const osg::CopyOp& = {})
            : osg::Object(copy)
            , mOccluderMeshes(copy.mOccluderMeshes)
        {
        }
        META_Object(MWRender, PagedOccluderData)

        std::vector<OccluderMesh> mOccluderMeshes;''',
    '''        PagedOccluderData(const PagedOccluderData& copy, const osg::CopyOp& = {})
            : osg::Object(copy)
            , mOccluderMeshes(copy.mOccluderMeshes)
            , mChunkBounds(copy.mChunkBounds)
        {
        }
        META_Object(MWRender, PagedOccluderData)

        std::vector<OccluderMesh> mOccluderMeshes;
        osg::BoundingBox mChunkBounds;''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''    /// Installed on each Cell Root group. Two-pass approach:''',
    '''    /// Lightweight whole-group rejection for render groups that never rasterize themselves as occluders.
    class CoarseOcclusionCallback
        : public SceneUtil::NodeCallback<CoarseOcclusionCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        CoarseOcclusionCallback(
            SceneUtil::OcclusionCuller* culler, const osg::BoundingBox& localBounds, bool groundcover);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

    private:
        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        osg::BoundingBox mLocalBounds;
        bool mGroundcover;
    };

    /// Installed on each Cell Root group. Two-pass approach:''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        OccluderMesh transformLocalMesh(const OccluderMesh& localMesh, const osg::Matrixf& matrix)
        {
            OccluderMesh worldMesh;
            worldMesh.indices = localMesh.indices;
            worldMesh.vertices.reserve(localMesh.vertices.size());
            for (const auto& v : localMesh.vertices)
            {
                const osg::Vec3f transformed = v * matrix;
                worldMesh.vertices.push_back(transformed);
                worldMesh.aabb.expandBy(transformed);
            }

            if (localMesh.vertices.empty() && localMesh.aabb.valid())
            {
                for (unsigned int i = 0; i < 8; ++i)
                    worldMesh.aabb.expandBy(localMesh.aabb.corner(i) * matrix);
            }
            return worldMesh;
        }''',
    '''        OccluderMesh transformLocalMesh(const OccluderMesh& localMesh, const osg::Matrixf& matrix)
        {
            OccluderMesh worldMesh;
            worldMesh.indices = localMesh.indices;
            worldMesh.vertices.reserve(localMesh.vertices.size());
            for (const auto& v : localMesh.vertices)
            {
                const osg::Vec3f transformed = v * matrix;
                worldMesh.vertices.push_back(transformed);
                worldMesh.aabb.expandBy(transformed);
            }

            if (localMesh.vertices.empty() && localMesh.aabb.valid())
            {
                for (unsigned int i = 0; i < 8; ++i)
                    worldMesh.aabb.expandBy(localMesh.aabb.corner(i) * matrix);
            }
            return worldMesh;
        }

        osg::BoundingBox transformLocalBounds(const osg::BoundingBox& localBounds, const osg::Matrixd& modelToWorld)
        {
            osg::BoundingBox worldBounds;
            if (!localBounds.valid())
                return worldBounds;
            for (unsigned int i = 0; i < 8; ++i)
                worldBounds.expandBy(localBounds.corner(i) * modelToWorld);
            return worldBounds;
        }''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        // Transform chunk bounding sphere from local to world space.
        // The chunk sits under a PAT, so node->getBound() is in chunk-local space.
        const osg::BoundingSphere& bs = node->getBound();
        if (bs.valid())
        {
            osg::Matrixd viewInverse;
            viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;
            const osg::Vec3f worldCenter = bs.center() * modelToWorld;
            const float r = bs.radius();

            osg::BoundingBox worldBB(worldCenter.x() - r, worldCenter.y() - r, worldCenter.z() - r, worldCenter.x() + r,
                worldCenter.y() + r, worldCenter.z() + r);

            // If entire chunk is occluded, skip rasterization AND traversal
            if (!mCuller->testVisibleAABB(worldBB))
                return;

            // Rasterize nearby building occluder meshes for visible chunks
            const osg::Vec3f eyeWorld(viewInverse(3, 0), viewInverse(3, 1), viewInverse(3, 2));

            if (auto* udc = node->getUserDataContainer())
            {
                for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
                {
                    if (auto* pod = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                    {
                        for (const auto& occMesh : pod->mOccluderMeshes)
                        {
                            if (occMesh.indices.empty())
                                continue;

                            const osg::Vec3f center = occMesh.aabb.center();
                            if ((center - eyeWorld).length2() > mMaxDistanceSq)
                                continue;

                            unsigned int newTris = static_cast<unsigned int>(occMesh.indices.size() / 3);
                            if (mMaxTriangles > 0 && mCuller->getNumBuildingTris() + newTris > mMaxTriangles)
                                continue;

                            mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);
                            mCuller->incrementBuildingOccluders(newTris,
                                static_cast<unsigned int>(occMesh.vertices.size()));
                        }
                        break;
                    }
                }
            }
        }

        traverse(node, cv);
    }

    CellOcclusionCallback::CellOcclusionCallback''',
    '''    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        osg::Matrixd viewInverse;
        viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;

        PagedOccluderData* pagedData = nullptr;
        if (auto* udc = node->getUserDataContainer())
        {
            for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
            {
                if (auto* candidate = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                {
                    pagedData = candidate;
                    break;
                }
            }
        }

        bool visible = true;
        if (pagedData && pagedData->mChunkBounds.valid())
        {
            const osg::BoundingBox worldBounds = transformLocalBounds(pagedData->mChunkBounds, modelToWorld);
            visible = mCuller->testVisibleCoarseAABB(
                worldBounds, SceneUtil::OcclusionTestCategory::PagedChunk);
        }
        else
        {
            const osg::BoundingSphere& bs = node->getBound();
            if (bs.valid())
            {
                const osg::Vec3f worldCenter = bs.center() * modelToWorld;
                const float r = bs.radius();
                const osg::BoundingBox worldBounds(worldCenter.x() - r, worldCenter.y() - r, worldCenter.z() - r,
                    worldCenter.x() + r, worldCenter.y() + r, worldCenter.z() + r);
                visible = mCuller->testVisibleAABB(worldBounds);
            }
        }
        if (!visible)
            return;

        if (pagedData)
        {
            const osg::Vec3f eyeWorld(viewInverse(3, 0), viewInverse(3, 1), viewInverse(3, 2));
            for (const auto& occMesh : pagedData->mOccluderMeshes)
            {
                if (occMesh.indices.empty())
                    continue;
                const osg::Vec3f center = occMesh.aabb.center();
                if ((center - eyeWorld).length2() > mMaxDistanceSq)
                    continue;
                const unsigned int newTris = static_cast<unsigned int>(occMesh.indices.size() / 3);
                if (mMaxTriangles > 0 && mCuller->getNumBuildingTris() + newTris > mMaxTriangles)
                    continue;
                mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);
                mCuller->incrementBuildingOccluders(
                    newTris, static_cast<unsigned int>(occMesh.vertices.size()));
            }
        }

        traverse(node, cv);
    }

    CoarseOcclusionCallback::CoarseOcclusionCallback(
        SceneUtil::OcclusionCuller* culler, const osg::BoundingBox& localBounds, bool groundcover)
        : mCuller(culler)
        , mLocalBounds(localBounds)
        , mGroundcover(groundcover)
    {
    }

    void CoarseOcclusionCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive() || !mLocalBounds.valid())
        {
            traverse(node, cv);
            return;
        }

        osg::Matrixd viewInverse;
        viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;
        const osg::BoundingBox worldBounds = transformLocalBounds(mLocalBounds, modelToWorld);
        const auto category = mGroundcover ? SceneUtil::OcclusionTestCategory::GroundcoverChunk
                                           : SceneUtil::OcclusionTestCategory::PagedChunk;
        if (mCuller->testVisibleCoarseAABB(worldBounds, category))
            traverse(node, cv);
    }

    CellOcclusionCallback::CellOcclusionCallback''',
)

# Object paging.
replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
            OcclusionCulling::OcclusionStorage* storage);''',
    '''        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
            OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion = false);''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        OcclusionCulling::OcclusionStorage* mOcclusionStorage = nullptr;
        bool mActiveGrid;''',
    '''        OcclusionCulling::OcclusionStorage* mOcclusionStorage = nullptr;
        bool mV35CoarseChunkOcclusion = false;
        bool mActiveGrid;''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''#include <osg/LOD>
#include <osg/MatrixTransform>''',
    '''#include <osg/ComputeBoundsVisitor>
#include <osg/LOD>
#include <osg/MatrixTransform>''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''    void ObjectPaging::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
        OcclusionCulling::OcclusionStorage* storage)
    {
        mOcclusionCuller = culler;
        mMaxTriangles = maxTriangles;
        mOcclusionStorage = storage;
    }''',
    '''    void ObjectPaging::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
        OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion)
    {
        mOcclusionCuller = culler;
        mMaxTriangles = maxTriangles;
        mOcclusionStorage = storage;
        mV35CoarseChunkOcclusion = coarseChunkOcclusion;
    }''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        if (buildOccluders)
        {
            pagedOccluderData = new PagedOccluderData;
            occluderMinRadius = Settings::camera().mOcclusionOccluderMinRadius;''',
    '''        if (buildOccluders || mV35CoarseChunkOcclusion)
            pagedOccluderData = new PagedOccluderData;
        if (buildOccluders)
        {
            occluderMinRadius = Settings::camera().mOcclusionOccluderMinRadius;''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        group->getBound();
        group->setNodeMask(Mask_Static);''',
    '''        group->getBound();
        if (mV35CoarseChunkOcclusion && pagedOccluderData)
        {
            osg::ComputeBoundsVisitor v35BoundsVisitor;
            group->accept(v35BoundsVisitor);
            pagedOccluderData->mChunkBounds = v35BoundsVisitor.getBoundingBox();
        }
        group->setNodeMask(Mask_Static);''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        if (pagedOccluderData && !pagedOccluderData->mOccluderMeshes.empty())
        {
            udc->addUserObject(pagedOccluderData);
            if (mOcclusionCuller)
            {
                float maxDist = Settings::camera().mOcclusionOccluderMaxDistance;
                group->addCullCallback(new PagedOccluderCallback(mOcclusionCuller, maxDist, mMaxTriangles));
            }
        }''',
    '''        if (pagedOccluderData
            && (!pagedOccluderData->mOccluderMeshes.empty()
                || (mV35CoarseChunkOcclusion && pagedOccluderData->mChunkBounds.valid())))
        {
            udc->addUserObject(pagedOccluderData);
            if (mOcclusionCuller)
            {
                const float maxDist = Settings::camera().mOcclusionOccluderMaxDistance;
                group->addCullCallback(new PagedOccluderCallback(mOcclusionCuller, maxDist, mMaxTriangles));
            }
        }''',
)

# Groundcover coarse culling.
replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''namespace osg
{
    class Program;
}

namespace MWRender''',
    '''namespace osg
{
    class Program;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace MWRender''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        struct GroundcoverEntry''',
    '''        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, bool coarseChunkOcclusion);

        struct GroundcoverEntry''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''        Resource::SceneManager* mSceneManager;
        float mDensity;''',
    '''        Resource::SceneManager* mSceneManager;
        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;
        bool mV35CoarseChunkOcclusion = false;
        float mDensity;''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''#include "groundcover.hpp"

#include <span>''',
    '''#include "groundcover.hpp"

#include "occlusionculling.hpp"

#include <span>''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''    Groundcover::~Groundcover() = default;

    void Groundcover::collectInstances''',
    '''    Groundcover::~Groundcover() = default;

    void Groundcover::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, bool coarseChunkOcclusion)
    {
        mOcclusionCuller = culler;
        mV35CoarseChunkOcclusion = coarseChunkOcclusion;
    }

    void Groundcover::collectInstances''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        osg::BoundingBox box = cbv.getBoundingBox();
        group->addCullCallback(new ViewDistanceCallback(getViewDistance(), box));

        group->setStateSet(mStateset);''',
    '''        osg::BoundingBox box = cbv.getBoundingBox();
        group->addCullCallback(new ViewDistanceCallback(getViewDistance(), box));
        if (mV35CoarseChunkOcclusion && mOcclusionCuller && box.valid())
            group->addCullCallback(new CoarseOcclusionCallback(mOcclusionCuller, box, true));

        group->setStateSet(mStateset);''',
)

# RenderingManager wiring.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''            if (mObjectPaging)
                mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles, mOcclusionStorage.get());
        }''',
    '''            if (mObjectPaging)
                mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles, mOcclusionStorage.get(),
                    Settings::camera().mV35CoarseChunkOcclusion);
            if (mGroundcover)
                mGroundcover->setOcclusionCuller(mOcclusionCuller, Settings::camera().mV35CoarseChunkOcclusion);
        }''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                    newChunkMgr.mObjectPaging->setOcclusionCuller(
                        mOcclusionCuller, maxTriangles, mOcclusionStorage.get());''',
    '''                    newChunkMgr.mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles,
                        mOcclusionStorage.get(), Settings::camera().mV35CoarseChunkOcclusion);''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());''',
    '''                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                if (mOcclusionCuller)
                    newChunkMgr.mGroundcover->setOcclusionCuller(
                        mOcclusionCuller, Settings::camera().mV35CoarseChunkOcclusion);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                chunkMgr.mObjectPaging->setOcclusionCuller(
                    mOcclusionCuller, maxTriangles, mOcclusionStorage.get());''',
    '''                chunkMgr.mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles,
                    mOcclusionStorage.get(), Settings::camera().mV35CoarseChunkOcclusion);''',
)

# Far shadows: bounded reuse with dynamic casters when explicitly enabled.
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift, settings.mActorShadows || settings.mPlayerShadows);''',
    '''        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift,
            (settings.mActorShadows || settings.mPlayerShadows) && !settings.mV35AllowDynamicFarCascadeReuse);''',
)

# Lua first-materialization attribution. No scheduling/order semantics change.
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''#include <components/esm/luascripts.hpp>''',
    '''#include <components/debug/v35lualoadtrace.hpp>
#include <components/esm/luascripts.hpp>''',
)

replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''        UnloadedData& unloadedData = std::get<UnloadedData>(mData);
        std::vector<ESM::LuaScript> savedScripts = std::move(unloadedData.mScripts);
        LoadedData& data = mData.emplace<LoadedData>();

        const ScriptsConfiguration& cfg = mLua.getConfiguration();

        std::map<int, ScriptInfo> scripts;
        for (const auto& [scriptId, initData] : mAutoStartScripts)
            scripts[scriptId] = { initData, nullptr };
        for (const ESM::LuaScript& s : savedScripts)
        {
            auto it = scripts.find(s.mScriptId);
            if (it != scripts.end())
                it->second.mSavedData = &s;
            else if (cfg.isCustomScript(s.mScriptId))
                scripts[s.mScriptId] = { cfg[s.mScriptId].mInitializationData, &s };
        }

        mLua.protectedCall([&](LuaView& view) {
            data.mPublicInterfaces = sol::table(view.sol(), sol::create);
            addPackage("openmw.interfaces", makeReadOnly(data.mPublicInterfaces));

            for (const auto& [scriptId, scriptInfo] : scripts)
            {
                std::optional<sol::function> onInit, onLoad;
                if (!addScript(view, scriptId, onInit, onLoad))
                    continue;
                if (scriptInfo.mSavedData == nullptr)
                {
                    if (onInit)
                        callOnInit(view, scriptId, *onInit, scriptInfo.mInitData);
                    continue;
                }
                if (onLoad)
                {
                    try
                    {
                        sol::object state = deserialize(view.sol(), scriptInfo.mSavedData->mData, mSerializer);
                        sol::object initializationData = deserialize(view.sol(), scriptInfo.mInitData, mSerializer);
                        LuaUtil::call({ this, scriptId }, *onLoad, state, initializationData);
                    }
                    catch (std::exception& e)
                    {
                        printError(scriptId, "onLoad failed", e);
                    }
                }
                for (const ESM::LuaTimer& savedTimer : scriptInfo.mSavedData->mTimers)
                {
                    Timer timer;
                    timer.mCallback = savedTimer.mCallbackName;
                    timer.mSerializable = true;
                    timer.mScriptId = scriptId;
                    timer.mTime = savedTimer.mTime;

                    try
                    {
                        timer.mArg
                            = sol::main_object(deserialize(view.sol(), savedTimer.mCallbackArgument, mSerializer));
                        // It is important if the order of content files was changed. The deserialize-serialize
                        // procedure updates refnums, so timer.mSerializedArg may be not equal to
                        // savedTimer.mCallbackArgument.
                        timer.mSerializedArg = serialize(timer.mArg, mSerializer);

                        if (savedTimer.mType == TimerType::GAME_TIME)
                            data.mGameTimersQueue.push_back(std::move(timer));
                        else
                            data.mSimulationTimersQueue.push_back(std::move(timer));
                    }
                    catch (std::exception& e)
                    {
                        printError(scriptId, "can not load timer", e);
                    }
                }
            }
        });

        std::make_heap(data.mSimulationTimersQueue.begin(), data.mSimulationTimersQueue.end());
        std::make_heap(data.mGameTimersQueue.begin(), data.mGameTimersQueue.end());

        if (mTracker)
            mTracker->onLoad(*this);''',
    '''        Debug::V35LuaLoadTrace::LoadScope v35LoadTrace(mNamePrefix);
        std::vector<ESM::LuaScript> savedScripts;
        LoadedData* dataPtr = nullptr;
        std::map<int, ScriptInfo> scripts;
        {
            Debug::V35LuaLoadTrace::PhaseScope v35Prepare(Debug::V35LuaLoadTrace::Phase::Prepare);
            UnloadedData& unloadedData = std::get<UnloadedData>(mData);
            savedScripts = std::move(unloadedData.mScripts);
            dataPtr = &mData.emplace<LoadedData>();

            const ScriptsConfiguration& cfg = mLua.getConfiguration();
            for (const auto& [scriptId, initData] : mAutoStartScripts)
                scripts[scriptId] = { initData, nullptr };
            for (const ESM::LuaScript& s : savedScripts)
            {
                auto it = scripts.find(s.mScriptId);
                if (it != scripts.end())
                    it->second.mSavedData = &s;
                else if (cfg.isCustomScript(s.mScriptId))
                    scripts[s.mScriptId] = { cfg[s.mScriptId].mInitializationData, &s };
            }
        }
        LoadedData& data = *dataPtr;
        Debug::V35LuaLoadTrace::setScriptCounts(
            static_cast<unsigned>(scripts.size()), static_cast<unsigned>(savedScripts.size()));

        mLua.protectedCall([&](LuaView& view) {
            {
                Debug::V35LuaLoadTrace::PhaseScope v35Interfaces(Debug::V35LuaLoadTrace::Phase::Interfaces);
                data.mPublicInterfaces = sol::table(view.sol(), sol::create);
                addPackage("openmw.interfaces", makeReadOnly(data.mPublicInterfaces));
            }

            for (const auto& [scriptId, scriptInfo] : scripts)
            {
                Debug::V35LuaLoadTrace::ScriptScope v35Script(scriptPath(scriptId).value());
                std::optional<sol::function> onInit, onLoad;
                bool added = false;
                {
                    Debug::V35LuaLoadTrace::PhaseScope v35Add(Debug::V35LuaLoadTrace::Phase::AddScripts);
                    added = addScript(view, scriptId, onInit, onLoad);
                }
                if (!added)
                    continue;
                if (scriptInfo.mSavedData == nullptr)
                {
                    Debug::V35LuaLoadTrace::PhaseScope v35Init(Debug::V35LuaLoadTrace::Phase::InitLoad);
                    if (onInit)
                        callOnInit(view, scriptId, *onInit, scriptInfo.mInitData);
                    continue;
                }
                if (onLoad)
                {
                    Debug::V35LuaLoadTrace::PhaseScope v35Load(Debug::V35LuaLoadTrace::Phase::InitLoad);
                    try
                    {
                        sol::object state = deserialize(view.sol(), scriptInfo.mSavedData->mData, mSerializer);
                        sol::object initializationData = deserialize(view.sol(), scriptInfo.mInitData, mSerializer);
                        LuaUtil::call({ this, scriptId }, *onLoad, state, initializationData);
                    }
                    catch (std::exception& e)
                    {
                        printError(scriptId, "onLoad failed", e);
                    }
                }
                {
                    Debug::V35LuaLoadTrace::PhaseScope v35Timers(Debug::V35LuaLoadTrace::Phase::Timers);
                    for (const ESM::LuaTimer& savedTimer : scriptInfo.mSavedData->mTimers)
                    {
                        Debug::V35LuaLoadTrace::addTimerCount();
                        Timer timer;
                        timer.mCallback = savedTimer.mCallbackName;
                        timer.mSerializable = true;
                        timer.mScriptId = scriptId;
                        timer.mTime = savedTimer.mTime;

                        try
                        {
                            timer.mArg
                                = sol::main_object(deserialize(view.sol(), savedTimer.mCallbackArgument, mSerializer));
                            timer.mSerializedArg = serialize(timer.mArg, mSerializer);

                            if (savedTimer.mType == TimerType::GAME_TIME)
                                data.mGameTimersQueue.push_back(std::move(timer));
                            else
                                data.mSimulationTimersQueue.push_back(std::move(timer));
                        }
                        catch (std::exception& e)
                        {
                            printError(scriptId, "can not load timer", e);
                        }
                    }
                }
            }
        });

        {
            Debug::V35LuaLoadTrace::PhaseScope v35Heap(Debug::V35LuaLoadTrace::Phase::Heap);
            std::make_heap(data.mSimulationTimersQueue.begin(), data.mSimulationTimersQueue.end());
            std::make_heap(data.mGameTimersQueue.begin(), data.mGameTimersQueue.end());
        }

        if (mTracker)
        {
            Debug::V35LuaLoadTrace::PhaseScope v35Tracker(Debug::V35LuaLoadTrace::Phase::Tracker);
            mTracker->onLoad(*this);
        }''',
)

# Unified benchmark: standardize density and add V3.5 options/streams.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    """$V34BroadenOcclusion = 'false'
$OccluderMinRadius = '400'
$OccluderMaxDistance = '6144'
$OcclusionMaxTriangles = '30000'""",
    """$V34BroadenOcclusion = 'false'
$V35CoarseChunkOcclusion = 'false'
$V35AllowDynamicFarReuse = 'false'
$OccluderMinRadius = '400'
$OccluderMaxDistance = '6144'
$OcclusionMaxTriangles = '30000'""",
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
do { $choice = Read-Host 'Enter 1 through 18' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18'))''',
    '''Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
Write-Host ' 19 = V3.5 coarse chunk MSOC (paged objects + groundcover)'
Write-Host ' 20 = V3.5 dynamic far reuse + divisor 4 (far cascade interval 2)'
Write-Host ' 21 = V3.5 coarse chunk MSOC + divisor 4'
Write-Host ' 22 = V3.5 coarse chunk MSOC + proven Lua fast path'
Write-Host ' 23 = V3.5 full combined: coarse MSOC + Lua fast + divisor 4 + dynamic far reuse'
do { $choice = Read-Host 'Enter 1 through 23' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
}''',
    '''    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '19' { $Experiment = 'v35-coarse-msoc'; $V35CoarseChunkOcclusion = 'true' }
    '20' { $Experiment = 'v35-dynamic-far-reuse'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
    '21' { $Experiment = 'v35-coarse-shadow'; $V35CoarseChunkOcclusion = 'true'; $FarShadowResolutionDivisor = '4' }
    '22' { $Experiment = 'v35-coarse-lua'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true' }
    '23' { $Experiment = 'v35-full-combined'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "occlusion_occluder_min_radius=$OccluderMinRadius",''',
    '''    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "v35_coarse_chunk_occlusion=$V35CoarseChunkOcclusion",
    "v35_allow_dynamic_far_reuse=$V35AllowDynamicFarReuse",
    "benchmark_groundcover_density=1.0",
    "occlusion_occluder_min_radius=$OccluderMinRadius",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    """    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_V33_LUA_CALLBACK_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'""",
    """    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_V33_LUA_CALLBACK_FILE',
    'OPENMW_V35_LUA_LOAD_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'""",
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    $env:OPENMW_V33_LUA_CALLBACK_FILE = Join-Path $ProfileDir 'v33-lua-callbacks.csv'
    $env:OPENMW_V3_TRANSITION_FILE''',
    '''    $env:OPENMW_V33_LUA_CALLBACK_FILE = Join-Path $ProfileDir 'v33-lua-callbacks.csv'
    $env:OPENMW_V35_LUA_LOAD_FILE = Join-Path $ProfileDir 'v35-lua-loads.csv'
    $env:OPENMW_V3_TRANSITION_FILE''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Shadows' 'v3.5 allow dynamic far cascade reuse' $V35AllowDynamicFarReuse
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius''',
    '''    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'v3.5 coarse chunk occlusion' $V35CoarseChunkOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Camera' 'occlusion max triangles' $OcclusionMaxTriangles
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
    '''    Set-IniValue $SettingsPath 'Camera' 'occlusion max triangles' $OcclusionMaxTriangles
    # Benchmark invariant: historical V3 comparison runs use groundcover density 1.0.
    # settings.cfg is restored in finally, so normal-play density is preserved after the run.
    Set-IniValue $SettingsPath 'Groundcover' 'density' '1.0'
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
)

print("V3.5 coarse MSOC, dynamic far reuse, Lua load attribution, and benchmark standardization completed successfully.")
