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
#include <iomanip>
#include <string>
#include <vector>

#include <components/occlusionculling/telemetry.hpp>

class MaskedOcclusionCulling;

namespace SceneUtil
{
    /// Wraps Intel's Masked Software Occlusion Culling library.
    /// Provides a CPU-based hierarchical depth buffer for occlusion testing during cull traversal.
    class OcclusionCuller : public osg::Referenced
    {
    public:
        OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight);
        ~OcclusionCuller();

        /// Call at the start of each frame's cull traversal.
        /// Clears the depth buffer and stores the view-projection matrix.
        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix);

        /// Rasterize terrain into the full buffer AND the terrain-only snapshot.
        /// Call this only for terrain, before any building occluders are added.
        void rasterizeTerrainOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).
        /// Use for buildings.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize a world-space AABB as an occluder (12 triangles for 6 faces).
        void rasterizeAABBOccluder(const osg::BoundingBox& worldBB);

        /// Test against the terrain-only depth buffer.
        /// Use for cells and buildings — prevents buildings from false-occluding each other.
        bool testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const;

        /// Test against the full depth buffer (terrain + buildings).
        /// Use for small objects in Pass 2.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

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

            mFrameActive = false;
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
        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;

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
                          "aabbs_tested,aabbs_occluded,rejection_pct,cache_mem_hits_total,cache_db_hits_total,"
                          "cache_misses_total,cache_writes_total\n";
            }

            const auto cache = OcclusionCulling::Telemetry::getLifetimeCacheStats();
            const double rejectionPct
                = mNumTested ? (100.0 * static_cast<double>(mNumOccluded) / static_cast<double>(mNumTested)) : 0.0;

            stream << ++mTelemetryFrameIndex << ',' << std::fixed << std::setprecision(3)
                   << (static_cast<double>(traverseNs) / 1000000.0) << ','
                   << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumBuildingVerts << ','
                   << mNumTested << ',' << mNumOccluded << ',' << rejectionPct << ','
                   << cache.memHits << ',' << cache.dbHits << ',' << cache.misses << ',' << cache.writes << '\n';

            if (++linesSinceFlush >= 300)
            {
                stream.flush();
                linesSinceFlush = 0;
            }
        }

        MaskedOcclusionCulling* mMOC;
        MaskedOcclusionCulling* mMOCTerrainOnly; // terrain-only snapshot for building visibility tests
        osg::Matrixd mViewProjection;
        float mVPFloat[16] = {};
        bool mFrameActive = false;

        mutable unsigned int mNumOccluded = 0;
        mutable unsigned int mNumTested = 0;
        unsigned int mNumBuildingOccluders = 0;
        unsigned int mNumBuildingTris = 0;
        unsigned int mNumBuildingVerts = 0;

        // V3 telemetry is opt-in. With the environment variable unset, no file is
        // opened and the existing hot path only pays the cost of a predictable branch.
        bool mTelemetryEnabled = [] {
            const char* path = std::getenv("OPENMW_V3_TELEMETRY_FILE");
            return path && *path;
        }();
        mutable bool mTelemetryFrameStarted = false;
        mutable std::chrono::steady_clock::time_point mTelemetryFrameStart{};
        std::uint64_t mTelemetryFrameIndex = 0;
    };
}

#endif
