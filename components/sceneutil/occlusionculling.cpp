#include "occlusionculling.hpp"

#include <MaskedOcclusionCulling.h>

#include <components/sceneutil/framejobservice.hpp>
#include <components/debug/v3deeptelemetry.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace SceneUtil
{
    struct OcclusionCuller::V323ParallelWorker
    {
        V323ParallelWorker()
            : mThread([this] { run(); })
        {
        }

        ~V323ParallelWorker()
        {
            {
                std::lock_guard lock(mMutex);
                mStop = true;
            }
            mWork.notify_one();
            if (mThread.joinable())
                mThread.join();
        }

        bool trySubmit(std::function<void()> task)
        {
            std::lock_guard lock(mMutex);
            if (mBusy || mStop)
                return false;
            mTask = std::move(task);
            mBusy = true;
            mWork.notify_one();
            return true;
        }

        void wait()
        {
            std::unique_lock lock(mMutex);
            mDone.wait(lock, [this] { return !mBusy; });
        }

    private:
        void run()
        {
            while (true)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(mMutex);
                    mWork.wait(lock, [this] { return mStop || mBusy; });
                    if (mStop)
                        return;
                    task = mTask;
                }
                task();
                {
                    std::lock_guard lock(mMutex);
                    mTask = {};
                    mBusy = false;
                }
                mDone.notify_all();
            }
        }

        std::mutex mMutex;
        std::condition_variable mWork;
        std::condition_variable mDone;
        std::function<void()> mTask;
        bool mBusy = false;
        bool mStop = false;
        std::thread mThread;
    };

    OcclusionCuller::OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight)
        : mMOC(nullptr)
        , mMOCTerrainOnly(nullptr)
    {
        // Width must be multiple of 8, height multiple of 4
        bufferWidth = (bufferWidth + 7) & ~7u;
        bufferHeight = (bufferHeight + 3) & ~3u;

        mMOC = MaskedOcclusionCulling::Create();
        if (mMOC)
        {
            mMOC->SetResolution(bufferWidth, bufferHeight);
            mMOC->SetNearClipPlane(0.1f);
        }
        mMOCTerrainOnly = MaskedOcclusionCulling::Create();
        if (mMOCTerrainOnly)
        {
            mMOCTerrainOnly->SetResolution(bufferWidth, bufferHeight);
            mMOCTerrainOnly->SetNearClipPlane(0.1f);
        }

        if (mV323ParallelMsocMode > 0 && mMOC && mMOCTerrainOnly)
        {
            mV323Worker = std::make_unique<V323ParallelWorker>();
            if (mV323ParallelMsocMode >= 2)
            {
                mV323WorkerMOC = MaskedOcclusionCulling::Create();
                if (mV323WorkerMOC)
                {
                    mV323WorkerMOC->SetResolution(bufferWidth, bufferHeight);
                    mV323WorkerMOC->SetNearClipPlane(0.1f);
                }
            }
        }
        if (mV324FrameJobQos && mV324AsyncMsoc && mMOC && mMOCTerrainOnly)
        {
            mV324WorkerMOC = MaskedOcclusionCulling::Create();
            if (mV324WorkerMOC)
            {
                mV324WorkerMOC->SetResolution(bufferWidth, bufferHeight);
                mV324WorkerMOC->SetNearClipPlane(0.1f);
            }
        }
    }

    OcclusionCuller::~OcclusionCuller()
    {
        if (mV324WorkerMOC)
        {
            if (mV324TerrainSubmittedGeneration != 0)
                FrameJobService::instance().wait(
                    FrameJobService::Lane::Opportunistic, mV324TerrainSubmittedGeneration);
            MaskedOcclusionCulling::Destroy(mV324WorkerMOC);
            mV324WorkerMOC = nullptr;
        }
        mV323Worker.reset();
        if (mV323WorkerMOC)
            MaskedOcclusionCulling::Destroy(mV323WorkerMOC);
        if (mMOC)
            MaskedOcclusionCulling::Destroy(mMOC);
        if (mMOCTerrainOnly)
            MaskedOcclusionCulling::Destroy(mMOCTerrainOnly);
    }

    void OcclusionCuller::beginFrame(
        const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, bool cacheViewInverse)
    {
        mFrameActive = false;
        mClearMs = 0.0;
        mTerrainBuildMs = 0.0;
        mTerrainRasterMs = 0.0;
        mBuildingRasterMs = 0.0;
        mAabbTotalMs = 0.0;
        mTestRectMs = 0.0;
        mDetailedTestCalls = 0;
        mPagedChunksTested = 0;
        mPagedChunksOccluded = 0;
        mGroundcoverChunksTested = 0;
        mGroundcoverChunksOccluded = 0;
        mPagedEstimatedChildrenSkipped = 0;
        mGroundcoverEstimatedInstancesSkipped = 0;
        if (!mMOC)
            return;

        const auto clearStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->ClearBuffer();
        if (mMOCTerrainOnly && !mV324AsyncMsoc)
            mMOCTerrainOnly->ClearBuffer();
        if (mV323WorkerMOC)
            mV323WorkerMOC->ClearBuffer();
        if (mV324AsyncMsoc)
        {
            ++mV324FrameGeneration;
            mV324TerrainSubmittedGeneration = 0;
        }
        if (mDetailedTelemetryEnabled)
            mClearMs = Debug::V3Diagnostics::elapsedMs(clearStart);
        mV322ViewInverseValid = false;
        if (cacheViewInverse)
        {
            mV322ViewInverseValid = mV322ViewInverse.invert(viewMatrix);
            if (mV322ViewInverseValid)
            {
                mV322EyeWorld = osg::Vec3f(static_cast<float>(mV322ViewInverse(3, 0)),
                    static_cast<float>(mV322ViewInverse(3, 1)), static_cast<float>(mV322ViewInverse(3, 2)));
            }
        }

        mViewProjection = viewMatrix * projectionMatrix;

        const double* vpDouble = mViewProjection.ptr();
        for (int i = 0; i < 16; ++i)
            mVPFloat[i] = static_cast<float>(vpDouble[i]);

        mNumOccluded = 0;
        mNumTested = 0;
        mNumBuildingOccluders = 0;
        mNumBuildingTris = 0;
        mNumBuildingVerts = 0;
        mFrameActive = true;
    }

    void OcclusionCuller::rasterizeTerrainOccluder(
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)
    {
        Debug::V324DeepTelemetry::Scope v324DeepScope("msoc", "terrain_rasterize_entry");

        if (mV324FrameJobQos && mV324AsyncMsoc && mV324WorkerMOC)
        {
            rasterizeOccluder(worldPositions, indices);

            FrameJobService& jobs = FrameJobService::instance();
            if (!jobs.isIdle(FrameJobService::Lane::Opportunistic))
            {
                jobs.noteSkipped(FrameJobService::Lane::Opportunistic);
                return;
            }

            std::shared_ptr<std::vector<osg::Vec3f>> positions;
            std::shared_ptr<std::vector<unsigned int>> ownedIndices;
            {
                Debug::V324DeepTelemetry::Scope scope("msoc", "async_input_copy");
                positions = std::make_shared<std::vector<osg::Vec3f>>(worldPositions);
                ownedIndices = std::make_shared<std::vector<unsigned int>>(indices);
            }
            std::array<float, 16> vp{};
            std::copy(std::begin(mVPFloat), std::end(mVPFloat), vp.begin());
            const std::uint64_t generation = mV324FrameGeneration;
            auto* readyGeneration = &mV324TerrainReadyGeneration;
            MaskedOcclusionCulling* workerMoc = mV324WorkerMOC;

            const bool submitted = jobs.trySubmit(FrameJobService::Lane::Opportunistic, generation,
                [workerMoc, positions = std::move(positions), ownedIndices = std::move(ownedIndices), vp,
                    readyGeneration, generation] {
                    workerMoc->ClearBuffer();
                    Debug::V324DeepTelemetry::Scope scope("msoc", "async_terrain_worker_raster");
                    if (OcclusionCuller::v324RenderOccluderTo(workerMoc, *positions, *ownedIndices, vp))
                        readyGeneration->store(generation, std::memory_order_release);
                });
            if (submitted)
                mV324TerrainSubmittedGeneration = generation;
            else
                jobs.noteSkipped(FrameJobService::Lane::Opportunistic);
            return;
        }

        Debug::V3Diagnostics::ScopedAccumulator timer(mDetailedTelemetryEnabled, mTerrainRasterMs);
        // Rasterize terrain into both buffers so buildings can be tested against
        // terrain-only depth (via testVisibleAABBTerrainOnly).
        mRasterizingTerrain = true;
        if (mV323ParallelMsocMode > 0 && mV323Worker && mMOCTerrainOnly)
        {
            const bool submitted = mV323Worker->trySubmit(
                [this, &worldPositions, &indices] { renderOccluderTo(mMOCTerrainOnly, worldPositions, indices); });
            // Main thread performs the exact existing full-buffer raster concurrently.
            rasterizeOccluder(worldPositions, indices);
            if (submitted)
            {
                // Bounded same-frame join on a dedicated worker. No shared preload queue participates.
                mV323Worker->wait();
                return;
            }
            // Busy-inline fallback: fall through to the inherited terrain-only raster body.
        }
        else
            rasterizeOccluder(worldPositions, indices);
        mRasterizingTerrain = false;
        if (!mFrameActive || !mMOCTerrainOnly || worldPositions.empty() || indices.empty())
            return;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return;

        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
            if (idx >= vertexCount)
                return;

        for (const auto& v : worldPositions)
        {
            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return;
        }

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);
        mMOCTerrainOnly->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris,
            mVPFloat, MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
    }

    void OcclusionCuller::rasterizeOccluder(
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)
    {
        if (!mFrameActive || worldPositions.empty() || indices.empty())
            return;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return;

        // Validate all vertex indices are in range to prevent out-of-bounds reads.
        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
        {
            if (idx >= vertexCount)
                return;
        }

        // Pre-transform check: skip triangles with vertices that would produce
        // extreme clip-space coordinates (w near zero after VP transform).
        for (const auto& v : worldPositions)
        {
            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return;
        }

        // Vec3f layout: stride=12 bytes, yOffset=4, zOffset=8
        // MOC treats (x,y,z) as (x,y,w_component) and transforms via the VP matrix
        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);

        const auto rasterStart
            = (mDetailedTelemetryEnabled && !mRasterizingTerrain) ? Debug::V3Diagnostics::Clock::now()
                                                                 : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // terrain can be seen from below at edges
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        if (mDetailedTelemetryEnabled && !mRasterizingTerrain)
            mBuildingRasterMs += Debug::V3Diagnostics::elapsedMs(rasterStart);
    }

    bool OcclusionCuller::v324RenderOccluderTo(MaskedOcclusionCulling* moc,
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices,
        const std::array<float, 16>& vp)
    {
        if (!moc || worldPositions.empty() || indices.empty())
            return false;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return false;

        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
            if (idx >= vertexCount)
                return false;

        for (const auto& v : worldPositions)
        {
            const float w = vp[3] * v.x() + vp[7] * v.y() + vp[11] * v.z() + vp[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return false;
        }

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);
        moc->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, vp.data(),
            MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        return true;
    }

    bool OcclusionCuller::renderOccluderTo(MaskedOcclusionCulling* moc,
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices) const
    {
        if (!mFrameActive || !moc || worldPositions.empty() || indices.empty())
            return false;

        const int numTris = static_cast<int>(indices.size()) / 3;
        if (numTris <= 0)
            return false;

        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());
        for (const auto idx : indices)
            if (idx >= vertexCount)
                return false;

        for (const auto& v : worldPositions)
        {
            const float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];
            if (!std::isfinite(w) || std::abs(w) < 1e-6f)
                return false;
        }

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);
        moc->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        return true;
    }

    void OcclusionCuller::rasterizeOccluderBatch(const std::vector<V323OccluderBatchView>& meshes)
    {
        if (meshes.empty())
            return;

        if (mV323ParallelMsocMode < 2 || !mV323Worker || !mV323WorkerMOC || meshes.size() < 2)
        {
            for (const auto& mesh : meshes)
                if (mesh.vertices && mesh.indices)
                    rasterizeOccluder(*mesh.vertices, *mesh.indices);
            return;
        }

        const bool submitted = mV323Worker->trySubmit([this, &meshes] {
            mV323WorkerMOC->ClearBuffer();
            for (std::size_t i = 1; i < meshes.size(); i += 2)
            {
                const auto& mesh = meshes[i];
                if (mesh.vertices && mesh.indices)
                    renderOccluderTo(mV323WorkerMOC, *mesh.vertices, *mesh.indices);
            }
        });
        if (!submitted)
        {
            // Busy-inline fallback: preserve exact current-frame visibility without queueing.
            for (const auto& mesh : meshes)
                if (mesh.vertices && mesh.indices)
                    rasterizeOccluder(*mesh.vertices, *mesh.indices);
            return;
        }

        for (std::size_t i = 0; i < meshes.size(); i += 2)
        {
            const auto& mesh = meshes[i];
            if (mesh.vertices && mesh.indices)
                rasterizeOccluder(*mesh.vertices, *mesh.indices);
        }
        mV323Worker->wait();
        mMOC->MergeBuffer(mV323WorkerMOC);
    }

    void OcclusionCuller::rasterizeAABBOccluder(const osg::BoundingBox& worldBB)
    {
        if (!mFrameActive)
            return;

        // 8 corners of the AABB
        const osg::Vec3f verts[8] = {
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMin()), // 0
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMin()), // 1
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMin()), // 2
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMin()), // 3
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMax()), // 4
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMax()), // 5
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMax()), // 6
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMax()), // 7
        };

        // 12 triangles (6 faces x 2 tris)
        static const unsigned int indices[36] = {
            // -Z face
            0,
            1,
            3,
            0,
            3,
            2,
            // +Z face
            4,
            6,
            7,
            4,
            7,
            5,
            // -Y face
            0,
            4,
            5,
            0,
            5,
            1,
            // +Y face
            2,
            3,
            7,
            2,
            7,
            6,
            // -X face
            0,
            2,
            6,
            0,
            6,
            4,
            // +X face
            1,
            5,
            7,
            1,
            7,
            3,
        };

        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);

        const auto rasterStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->RenderTriangles(reinterpret_cast<const float*>(verts), indices, 12, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // both sides, nearest depth wins
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        if (mDetailedTelemetryEnabled)
            mBuildingRasterMs += Debug::V3Diagnostics::elapsedMs(rasterStart);

        ++mNumBuildingOccluders;
    }

    bool OcclusionCuller::testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const
    {
        Debug::V3Diagnostics::ScopedAccumulator totalTimer(mDetailedTelemetryEnabled, mAabbTotalMs);
        if (mDetailedTelemetryEnabled)
            ++mDetailedTestCalls;

        const osg::Vec3f corners[8] = {
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMin()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMin(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMin(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMin(), worldBB.yMax(), worldBB.zMax()),
            osg::Vec3f(worldBB.xMax(), worldBB.yMax(), worldBB.zMax()),
        };

        float ndcMinX = 1.0f, ndcMinY = 1.0f;
        float ndcMaxX = -1.0f, ndcMaxY = -1.0f;
        float wMin = std::numeric_limits<float>::max();
        bool anyInFront = false;

        const double* m = mViewProjection.ptr();
        for (int i = 0; i < 8; ++i)
        {
            // Transform to clip space: pos * ViewProjection
            double x = corners[i].x(), y = corners[i].y(), z = corners[i].z();
            float cx = static_cast<float>(x * m[0] + y * m[4] + z * m[8] + m[12]);
            float cy = static_cast<float>(x * m[1] + y * m[5] + z * m[9] + m[13]);
            float cw = static_cast<float>(x * m[3] + y * m[7] + z * m[11] + m[15]);

            if (cw > 0.0f)
            {
                anyInFront = true;
                float invW = 1.0f / cw;
                float ndcX = cx * invW;
                float ndcY = cy * invW;
                ndcMinX = std::min(ndcMinX, ndcX);
                ndcMinY = std::min(ndcMinY, ndcY);
                ndcMaxX = std::max(ndcMaxX, ndcX);
                ndcMaxY = std::max(ndcMaxY, ndcY);
                wMin = std::min(wMin, cw);
            }
            else
            {
                // Corner is behind camera — conservatively expand to full screen
                ndcMinX = -1.0f;
                ndcMinY = -1.0f;
                ndcMaxX = 1.0f;
                ndcMaxY = 1.0f;
                anyInFront = true; // still test it
                wMin = std::min(wMin, 0.0001f);
            }
        }

        if (!anyInFront)
            return true; // entirely behind camera, let frustum culling handle it

        // Clamp to NDC range
        ndcMinX = std::max(ndcMinX, -1.0f);
        ndcMinY = std::max(ndcMinY, -1.0f);
        ndcMaxX = std::min(ndcMaxX, 1.0f);
        ndcMaxY = std::min(ndcMaxY, 1.0f);

        if (ndcMinX >= ndcMaxX || ndcMinY >= ndcMaxY)
            return true; // degenerate rect, assume visible

        const auto testStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        auto result = moc->TestRect(ndcMinX, ndcMinY, ndcMaxX, ndcMaxY, wMin);
        if (mDetailedTelemetryEnabled)
            mTestRectMs += Debug::V3Diagnostics::elapsedMs(testStart);
        return result != MaskedOcclusionCulling::OCCLUDED;
    }

    bool OcclusionCuller::testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const
    {
        if (mV324FrameJobQos && mV324AsyncMsoc)
        {
            if (!mFrameActive || !mV324WorkerMOC)
                return true;

            const std::uint64_t generation = mV324FrameGeneration;
            FrameJobService& jobs = FrameJobService::instance();
            if (mV324TerrainSubmittedGeneration != generation
                || mV324TerrainReadyGeneration.load(std::memory_order_acquire) != generation
                || !jobs.isComplete(FrameJobService::Lane::Opportunistic, generation)
                || jobs.failed(FrameJobService::Lane::Opportunistic, generation))
                return true; // incomplete/stale/failed parallel depth is fail-open only

            return testVisibleAABBImpl(mV324WorkerMOC, worldBB);
        }

        if (!mFrameActive || !mMOCTerrainOnly)
            return true;
        // No mNumTested/mNumOccluded tracking for terrain-only tests — those stats
        // are for the main (full) buffer tests used to measure culling effectiveness.
        return testVisibleAABBImpl(mMOCTerrainOnly, worldBB);
    }

    bool OcclusionCuller::testVisibleAABB(const osg::BoundingBox& worldBB) const
    {
        if (!mFrameActive)
            return true;

        ++mNumTested;
        const bool visible = testVisibleAABBImpl(mMOC, worldBB);
        if (!visible)
            ++mNumOccluded;
        return visible;
    }

    bool OcclusionCuller::testVisibleCoarseAABB(const osg::BoundingBox& worldBB,
        OcclusionTestCategory category, std::uint64_t estimatedChildren) const
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
            if (category == OcclusionTestCategory::PagedChunk)
                mPagedEstimatedChildrenSkipped += estimatedChildren;
            else
                mGroundcoverEstimatedInstancesSkipped += estimatedChildren;
        }
        return visible;
    }

    void OcclusionCuller::computePixelDepthBuffer(float* depthData) const
    {
        if (mMOC)
            mMOC->ComputePixelDepthBuffer(depthData, true); // flipY for OpenGL bottom-to-top
    }

    void OcclusionCuller::getResolution(unsigned int& width, unsigned int& height) const
    {
        if (mMOC)
            mMOC->GetResolution(width, height);
        else
        {
            width = 0;
            height = 0;
        }
    }
}
