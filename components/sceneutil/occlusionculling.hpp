#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Referenced>
#include <osg/Vec3f>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <array>
#include <atomic>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <components/debug/v3diagnostics.hpp>
#include <components/occlusionculling/telemetry.hpp>

class MaskedOcclusionCulling;

namespace SceneUtil
{
    enum class OcclusionTestCategory
    {
        PagedChunk,
        GroundcoverChunk,
    };

    /// Wraps Intel's Masked Software Occlusion Culling library.
    /// Provides a CPU-based hierarchical depth buffer for occlusion testing during cull traversal.
    class OcclusionCuller : public osg::Referenced
    {
    public:
        OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight);
        ~OcclusionCuller();

        /// Call at the start of each frame's cull traversal.
        /// Clears the depth buffer and stores the view-projection matrix.
        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix,
            bool cacheViewInverse = false);

        bool hasCachedViewInverse() const { return mV322ViewInverseValid; }
        const osg::Matrixd& getCachedViewInverse() const { return mV322ViewInverse; }
        const osg::Vec3f& getCachedEyeWorld() const { return mV322EyeWorld; }

        /// Rasterize terrain into the full buffer AND the terrain-only snapshot.
        /// Call this only for terrain, before any building occluders are added.
        void rasterizeTerrainOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).
        /// Use for buildings.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        struct V323OccluderBatchView
        {
            const std::vector<osg::Vec3f>* vertices = nullptr;
            const std::vector<unsigned int>* indices = nullptr;
        };

        /// V3.23 Mode143/144: split an immutable paged-occluder batch between the
        /// main MOC and a worker-private MOC, then conservatively merge after the worker completes.
        void rasterizeOccluderBatch(const std::vector<V323OccluderBatchView>& meshes);

        /// Rasterize a world-space AABB as an occluder (12 triangles for 6 faces).
        void rasterizeAABBOccluder(const osg::BoundingBox& worldBB);

        /// Test against the terrain-only depth buffer.
        /// Use for cells and buildings — prevents buildings from false-occluding each other.
        bool testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const;

        /// Test against the full depth buffer (terrain + buildings).
        /// Use for small objects in Pass 2.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        /// Test a coarse rendering group against the full buffer while retaining category telemetry.
        bool testVisibleCoarseAABB(const osg::BoundingBox& worldBB, OcclusionTestCategory category,
            std::uint64_t estimatedChildren = 0) const;

        bool isActive() const { return mMOC != nullptr; }
        bool isFrameActive() const
        {
            if (mFrameActive && mTelemetryEnabled && !mTelemetryFrameStarted)
            {
                mTelemetryFrameStart = std::chrono::steady_clock::now();
                mTelemetryFrameStarted = true;
            }
            return mFrameActive;
        }

        void endFrame()
        {
            if (!mFrameActive)
                return;

            if (mTelemetryEnabled)
            {
                std::uint64_t traverseNs = 0;
                if (mTelemetryFrameStarted)
                {
                    traverseNs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - mTelemetryFrameStart)
                            .count());
                }
                writeTelemetryRow(traverseNs);
                mTelemetryFrameStarted = false;
            }

            if (mDetailedTelemetryEnabled)
                writeDetailedTelemetryRow();

            mFrameActive = false;
        }

        void setTelemetryFrameNumber(unsigned int frameNumber) { mExternalFrameNumber = frameNumber; }
        bool detailedTelemetryEnabled() const { return mDetailedTelemetryEnabled; }
        void addTerrainBuildTime(double milliseconds)
        {
            if (mDetailedTelemetryEnabled)
                mTerrainBuildMs += milliseconds;
        }

        unsigned int getNumOccluded() const { return mNumOccluded; }
        unsigned int getNumTested() const { return mNumTested; }
        unsigned int getNumBuildingOccluders() const { return mNumBuildingOccluders; }
        unsigned int getNumBuildingTris() const { return mNumBuildingTris; }
        unsigned int getNumBuildingVerts() const { return mNumBuildingVerts; }
        void incrementBuildingOccluders(unsigned int tris, unsigned int verts)
        {
            ++mNumBuildingOccluders;
            mNumBuildingTris += tris;
            mNumBuildingVerts += verts;
        }

        /// Write the per-pixel depth buffer to depthData (width*height floats, bottom-to-top).
        void computePixelDepthBuffer(float* depthData) const;

        void getResolution(unsigned int& width, unsigned int& height) const;

    private:
        struct V323ParallelWorker;

        static bool v324RenderOccluderTo(MaskedOcclusionCulling* moc,
            const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices,
            const std::array<float, 16>& vp);
        bool renderOccluderTo(MaskedOcclusionCulling* moc, const std::vector<osg::Vec3f>& worldPositions,
            const std::vector<unsigned int>& indices) const;
        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;

        void writeDetailedTelemetryRow()
        {
            static Debug::V3Diagnostics::CsvWriter writer("OPENMW_V3_MSOC_DETAIL_FILE",
                "frame,epoch_ms,clear_ms,terrain_build_ms,terrain_raster_ms,building_raster_ms,"
                "aabb_total_ms,testrect_ms,test_calls,building_occluders,building_tris,aabbs_tested,aabbs_occluded,"
                "paged_chunks_tested,paged_chunks_occluded,groundcover_chunks_tested,groundcover_chunks_occluded,"
                "paged_estimated_children_skipped,groundcover_estimated_instances_skipped");
            if (!writer.enabled())
                return;

            std::ostringstream row;
            row << mExternalFrameNumber << ',' << Debug::V3Diagnostics::epochMs() << ',' << std::fixed
                << std::setprecision(3) << mClearMs << ',' << mTerrainBuildMs << ',' << mTerrainRasterMs << ','
                << mBuildingRasterMs << ',' << mAabbTotalMs << ',' << mTestRectMs << ',' << mDetailedTestCalls << ','
                << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumTested << ',' << mNumOccluded << ','
                << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                << mGroundcoverChunksOccluded << ',' << mPagedEstimatedChildrenSkipped << ','
                << mGroundcoverEstimatedInstancesSkipped;
            writer.writeLine(row.str());
        }

        void writeTelemetryRow(std::uint64_t traverseNs)
        {
            const char* path = std::getenv("OPENMW_V3_TELEMETRY_FILE");
            if (!path || !*path)
                return;

            static std::ofstream stream;
            static std::string openedPath;
            static unsigned int linesSinceFlush = 0;

            if (!stream.is_open() || openedPath != path)
            {
                if (stream.is_open())
                    stream.close();
                stream.clear();
                openedPath = path;
                stream.open(openedPath, std::ios::out | std::ios::trunc);
                if (!stream.is_open())
                    return;

                stream << "occlusion_frame,cull_traverse_ms,building_occluders,building_tris,building_verts,"
                          "aabbs_tested,aabbs_occluded,rejection_pct,paged_chunks_tested,paged_chunks_occluded,"
                          "groundcover_chunks_tested,groundcover_chunks_occluded,paged_estimated_children_skipped,"
                          "groundcover_estimated_instances_skipped,cache_mem_hits_total,cache_db_hits_total,"
                          "cache_misses_total,cache_writes_total\n";
            }

            const auto cache = OcclusionCulling::Telemetry::getLifetimeCacheStats();
            const double rejectionPct
                = mNumTested ? (100.0 * static_cast<double>(mNumOccluded) / static_cast<double>(mNumTested)) : 0.0;

            stream << ++mTelemetryFrameIndex << ',' << std::fixed << std::setprecision(3)
                   << (static_cast<double>(traverseNs) / 1000000.0) << ','
                   << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumBuildingVerts << ','
                   << mNumTested << ',' << mNumOccluded << ',' << rejectionPct << ','
                   << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                   << mGroundcoverChunksOccluded << ',' << mPagedEstimatedChildrenSkipped << ','
                   << mGroundcoverEstimatedInstancesSkipped << ',' << cache.memHits << ',' << cache.dbHits << ','
                   << cache.misses
                   << ',' << cache.writes << '\n';

            if (++linesSinceFlush >= 300)
            {
                stream.flush();
                linesSinceFlush = 0;
            }
        }

        MaskedOcclusionCulling* mMOC;
        MaskedOcclusionCulling* mMOCTerrainOnly; // terrain-only snapshot for building visibility tests
        osg::Matrixd mViewProjection;
        osg::Matrixd mV322ViewInverse;
        osg::Vec3f mV322EyeWorld;
        bool mV322ViewInverseValid = false;
        float mVPFloat[16] = {};
        int mV323ParallelMsocMode = [] {
            const char* value = std::getenv("OPENMW_V323_PARALLEL_MSOC_MODE");
            return value && value[0] >= '1' && value[0] <= '3' && value[1] == '\0' ? value[0] - '0' : 0;
        }();
        std::unique_ptr<V323ParallelWorker> mV323Worker;
        MaskedOcclusionCulling* mV323WorkerMOC = nullptr;
        bool mV324FrameJobQos = [] {
            const char* value = std::getenv("OPENMW_V324_FRAME_JOB_QOS");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        bool mV324AsyncMsoc = [] {
            const char* value = std::getenv("OPENMW_V324_ASYNC_MSOC");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        MaskedOcclusionCulling* mV324WorkerMOC = nullptr;
        std::uint64_t mV324FrameGeneration = 0;
        std::uint64_t mV324TerrainSubmittedGeneration = 0;
        std::atomic<std::uint64_t> mV324TerrainReadyGeneration{ 0 };
        bool mFrameActive = false;

        mutable unsigned int mNumOccluded = 0;
        mutable unsigned int mNumTested = 0;
        unsigned int mNumBuildingOccluders = 0;
        unsigned int mNumBuildingTris = 0;
        unsigned int mNumBuildingVerts = 0;
        mutable unsigned int mPagedChunksTested = 0;
        mutable unsigned int mPagedChunksOccluded = 0;
        mutable unsigned int mGroundcoverChunksTested = 0;
        mutable unsigned int mGroundcoverChunksOccluded = 0;
        mutable std::uint64_t mPagedEstimatedChildrenSkipped = 0;
        mutable std::uint64_t mGroundcoverEstimatedInstancesSkipped = 0;

        // V3 telemetry is opt-in. With the environment variable unset, no file is
        // opened and the existing hot path only pays the cost of a predictable branch.
        bool mTelemetryEnabled = [] {
            const char* path = std::getenv("OPENMW_V3_TELEMETRY_FILE");
            return path && *path;
        }();
        mutable bool mTelemetryFrameStarted = false;
        mutable std::chrono::steady_clock::time_point mTelemetryFrameStart{};
        std::uint64_t mTelemetryFrameIndex = 0;

        bool mDetailedTelemetryEnabled = [] {
            const char* path = std::getenv("OPENMW_V3_MSOC_DETAIL_FILE");
            return path && Debug::V3Diagnostics::pathEnabled(path);
        }();
        unsigned int mExternalFrameNumber = 0;
        double mClearMs = 0.0;
        double mTerrainBuildMs = 0.0;
        double mTerrainRasterMs = 0.0;
        mutable double mBuildingRasterMs = 0.0;
        mutable double mAabbTotalMs = 0.0;
        mutable double mTestRectMs = 0.0;
        mutable std::uint64_t mDetailedTestCalls = 0;
        bool mRasterizingTerrain = false;
    };
}

#endif
