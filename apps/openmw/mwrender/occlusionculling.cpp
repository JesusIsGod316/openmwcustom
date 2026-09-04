#include "occlusionculling.hpp"

#include "objects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/Group>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/misc/constants.hpp>
#include <components/occlusionculling/occludermesh.hpp>
#include <components/sceneutil/occlusionculling.hpp>
#include <components/terrain/terrainoccluder.hpp>
#include "../mwworld/class.hpp"

namespace MWRender
{
    namespace
    {
        bool v322MsocHotPathEnabled()
        {
            const char* value = std::getenv("OPENMW_V322_CP1_MSOC_HOT_PATH");
            return value && value[0] == '1';
        }

        int v322Cp2OccluderMode()
        {
            const char* value = std::getenv("OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE");
            if (!value || value[0] < '1' || value[0] > '4' || value[1] != '\0')
                return 0;
            return value[0] - '0';
        }

        int v323ParallelMsocMode()
        {
            const char* value = std::getenv("OPENMW_V323_PARALLEL_MSOC_MODE");
            if (!value || value[0] < '1' || value[0] > '3' || value[1] != '\0')
                return 0;
            return value[0] - '0';
        }

        std::string_view getModelPathForNode(osg::Node* node)
        {
            if (!node)
                return {};

            if (auto* udc = node->getUserDataContainer())
            {
                for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
                {
                    if (auto* holder = dynamic_cast<PtrHolder*>(udc->getUserObject(i)))
                        return holder->mPtr.getClass().getCorrectedModel(holder->mPtr);
                }
            }
            return {};
        }

        OccluderMesh transformLocalMesh(const OccluderMesh& localMesh, const osg::Matrixf& matrix)
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
        }
    }

    SceneOcclusionCallback::SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler,
        Terrain::TerrainOccluder* occluder, int radiusCells, bool enableTerrainOccluder, bool enableDebugOverlay,
        bool enableDebugMessages, bool enableInteriors, OcclusionStorage* storage)
        : mCuller(culler)
        , mTerrainOccluder(occluder)
        , mRadiusCells(radiusCells)
        , mEnableTerrainOccluder(enableTerrainOccluder)
        , mEnableDebugOverlay(enableDebugOverlay)
        , mEnableDebugMessages(enableDebugMessages)
        , mEnableInteriors(enableInteriors)
        , mV322MsocHotPath(v322MsocHotPathEnabled())
        , mStorage(storage)
    {
    }

    void SceneOcclusionCallback::setCellType(bool isInterior, bool isQuasiExterior)
    {
        mIsInterior = isInterior;
        mIsQuasiExterior = isQuasiExterior;
    }

    void SceneOcclusionCallback::setupDebugOverlay()
    {
        unsigned int w, h;
        mCuller->getResolution(w, h);
        if (w == 0 || h == 0)
            return;

        mDepthPixels.resize(w * h);

        // Create image to hold depth data (luminance float -> converted to RGBA)
        mDebugImage = new osg::Image;
        mDebugImage->allocateImage(w, h, 1, GL_LUMINANCE, GL_FLOAT);

        // Create texture from image
        mDebugTexture = new osg::Texture2D(mDebugImage);
        mDebugTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        mDebugTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mDebugTexture->setResizeNonPowerOfTwoHint(false);

        // Create POST_RENDER camera in corner of screen
        mDebugCamera = new osg::Camera;
        mDebugCamera->setName("OcclusionDebugCamera");
        mDebugCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        mDebugCamera->setRenderOrder(osg::Camera::POST_RENDER, 100);
        mDebugCamera->setAllowEventFocus(false);
        mDebugCamera->setClearMask(0);
        mDebugCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, 1, 0, 1));
        mDebugCamera->setViewMatrix(osg::Matrix::identity());
        mDebugCamera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        mDebugCamera->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        mDebugCamera->setCullingActive(false);

        // Scale viewport to show in bottom-left corner (400px wide, aspect-correct height)
        float displayWidth = 400.0f;
        float displayHeight = displayWidth * static_cast<float>(h) / static_cast<float>(w);
        mDebugCamera->setViewport(0, 0, static_cast<int>(displayWidth), static_cast<int>(displayHeight));

        // Create textured quad
        osg::ref_ptr<osg::Geometry> quad
            = osg::createTexturedQuadGeometry(osg::Vec3(0, 0, 0), osg::Vec3(1, 0, 0), osg::Vec3(0, 1, 0));
        quad->setCullingActive(false);

        osg::StateSet* ss = quad->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, mDebugTexture, osg::StateAttribute::ON);

        mDebugCamera->addChild(quad);
    }

    void SceneOcclusionCallback::updateDebugOverlay(osgUtil::CullVisitor* cv)
    {
        if (!mDebugCamera)
            return;

        unsigned int w, h;
        mCuller->getResolution(w, h);

        // Read depth buffer from MOC
        mCuller->computePixelDepthBuffer(mDepthPixels.data());

        // Copy to image (normalize: MOC stores 1/w, so closer = larger values)
        float* imageData = reinterpret_cast<float*>(mDebugImage->data());
        for (unsigned int i = 0; i < w * h; ++i)
        {
            float d = mDepthPixels[i];
            // MOC depth is 1/w (reciprocal clip-space w). 0 = far/empty, larger = closer.
            // Clamp and invert for visualization: dark = far, bright = near
            imageData[i] = std::min(d * 50.0f, 1.0f);
        }
        mDebugImage->dirty();

        // Inject debug camera into the cull visitor so it gets rendered
        unsigned int traversalMask = cv->getTraversalMask();
        cv->setTraversalMask(0xffffffff);
        mDebugCamera->accept(*cv);
        cv->setTraversalMask(traversalMask);
    }

    void SceneOcclusionCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        // Only run occlusion for the main scene camera.
        // Skip shadow cameras, water reflection, and any other cameras.
        osg::Camera* cam = cv->getCurrentCamera();
        if (cam->getName() != Constants::SceneCamera)
        {
            traverse(node, cv);
            return;
        }

        // The scene is traversed multiple times per frame: once for the main cull pass,
        // and again by MWShadowTechnique::cullShadowReceivingScene (same camera name).
        // Only set up MOC on the first traversal; subsequent passes just traverse normally.
        unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
        if (frameNumber == mLastFrameNumber)
        {
            traverse(node, cv);
            return;
        }
        mLastFrameNumber = frameNumber;

        // Skip MSOC entirely in interiors (unless enabled via setting)
        if (mIsInterior && !mEnableInteriors)
        {
            traverse(node, cv);
            return;
        }

        // Begin occlusion frame with camera matrices
        mCuller->setTelemetryFrameNumber(frameNumber);
        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix(), mV322MsocHotPath);

        // Build and rasterize terrain occluder mesh (skip for quasi-exteriors and interiors — no real terrain)
        if (mEnableTerrainOccluder && !mIsQuasiExterior && !mIsInterior && mTerrainOccluder->hasTerrainData())
        {
            mPositions.clear();
            mIndices.clear();
            const auto terrainBuildStart = mCuller->detailedTelemetryEnabled()
                ? Debug::V3Diagnostics::Clock::now()
                : Debug::V3Diagnostics::Clock::time_point{};
            mTerrainOccluder->build(cv->getEyePoint(), mRadiusCells, mPositions, mIndices);
            if (mCuller->detailedTelemetryEnabled())
                mCuller->addTerrainBuildTime(Debug::V3Diagnostics::elapsedMs(terrainBuildStart));

            if (!mPositions.empty())
                mCuller->rasterizeTerrainOccluder(mPositions, mIndices);
        }

        // Continue normal cull traversal — CellOcclusionCallbacks will test against the buffer
        traverse(node, cv);

        // End the occlusion frame so sub-camera traversals (water reflection/refraction,
        // shadow cameras) that share this scene graph don't incorrectly cull against
        // the main camera's occlusion buffer.
        mCuller->endFrame();

        // Update debug overlay AFTER traversal (terrain + building occluders now in buffer)
        if (mEnableDebugOverlay)
        {
            if (!mDebugCamera)
                setupDebugOverlay();
            updateDebugOverlay(cv);
        }

        if (mEnableDebugMessages)
        {
            static int frameCount = 0;
            if (++frameCount % 300 == 0)
            {
                const auto terrainTris = mIndices.size() / 3;
                const auto bldgTris = mCuller->getNumBuildingTris();
                const auto terrainVerts = mPositions.size();
                const auto bldgVerts = mCuller->getNumBuildingVerts();
                Log(Debug::Info) << "OcclusionCull: terrain tris=" << terrainTris << " terrain verts=" << terrainVerts
                                 << " bldg occluders=" << mCuller->getNumBuildingOccluders()
                                 << " bldg tris=" << bldgTris << " bldg verts=" << bldgVerts
                                 << " total tris=" << (terrainTris + bldgTris)
                                 << " total verts=" << (terrainVerts + bldgVerts)
                                 << " tested=" << mCuller->getNumTested()
                                 << " occluded=" << mCuller->getNumOccluded();
                if (mStorage)
                {
                    const auto s = mStorage->getAndResetStats();
                    Log(Debug::Info) << "OcclusionCache: mem_hits=" << s.memHits
                                     << " db_hits=" << s.dbHits
                                     << " misses(built)=" << s.misses
                                     << " writes=" << s.writes;
                }
            }
        }
    }

    PagedOccluderCallback::PagedOccluderCallback(
        SceneUtil::OcclusionCuller* culler, float maxDistance, unsigned int maxTriangles)
        : mCuller(culler)
        , mMaxDistanceSq(maxDistance * maxDistance)
        , mMaxTriangles(maxTriangles)
        , mV322MsocHotPath(v322MsocHotPathEnabled())
        , mV323ParallelMsocMode(v323ParallelMsocMode())
    {
        if (mV323ParallelMsocMode >= 2)
        {
            const float distanceScale = mV323ParallelMsocMode >= 3 ? 2.0f : 1.5f;
            mMaxDistanceSq *= distanceScale * distanceScale;
            if (mMaxTriangles > 0)
            {
                const unsigned int numerator = mV323ParallelMsocMode >= 3 ? 2u : 3u;
                const unsigned int denominator = mV323ParallelMsocMode >= 3 ? 1u : 2u;
                mMaxTriangles = (mMaxTriangles * numerator) / denominator;
            }
        }
    }

    void PagedOccluderCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        const bool useCachedView = mV322MsocHotPath && mCuller->hasCachedViewInverse();
        osg::Matrixd localViewInverse;
        const osg::Matrixd* viewInverse = nullptr;
        if (useCachedView)
            viewInverse = &mCuller->getCachedViewInverse();
        else
        {
            localViewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            viewInverse = &localViewInverse;
        }
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * (*viewInverse);

        std::vector<SceneUtil::OcclusionCuller::V323OccluderBatchView> v323Batch;
        PagedOccluderData* pagedData = nullptr;
        if (mV322MsocHotPath && mV322PagedDataNode == node && mV322PagedData)
            pagedData = mV322PagedData.get();
        else if (auto* udc = node->getUserDataContainer())
        {
            for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
            {
                if (auto* candidate = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                {
                    pagedData = candidate;
                    if (mV322MsocHotPath)
                    {
                        mV322PagedDataNode = node;
                        mV322PagedData = candidate;
                    }
                    break;
                }
            }
        }

        bool visible = true;
        if (pagedData && pagedData->mChunkBounds.valid())
        {
            const osg::BoundingBox worldBounds = transformLocalBounds(pagedData->mChunkBounds, modelToWorld);
            visible = mCuller->testVisibleCoarseAABB(worldBounds,
                SceneUtil::OcclusionTestCategory::PagedChunk, pagedData->mEstimatedChildren);
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
            const osg::Vec3f eyeWorld = useCachedView
                ? mCuller->getCachedEyeWorld()
                : osg::Vec3f(static_cast<float>((*viewInverse)(3, 0)),
                    static_cast<float>((*viewInverse)(3, 1)), static_cast<float>((*viewInverse)(3, 2)));
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
                if (mV323ParallelMsocMode >= 2)
                    v323Batch.push_back({ &occMesh.vertices, &occMesh.indices });
                else
                    mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);
                mCuller->incrementBuildingOccluders(
                    newTris, static_cast<unsigned int>(occMesh.vertices.size()));
            }
        }

        if (!v323Batch.empty())
            mCuller->rasterizeOccluderBatch(v323Batch);

        traverse(node, cv);
    }

    CoarseOcclusionCallback::CoarseOcclusionCallback(SceneUtil::OcclusionCuller* culler,
        const osg::BoundingBox& localBounds, bool groundcover, std::uint64_t estimatedChildren)
        : mCuller(culler)
        , mLocalBounds(localBounds)
        , mGroundcover(groundcover)
        , mEstimatedChildren(estimatedChildren)
    {
    }

    void CoarseOcclusionCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mCuller->isFrameActive() || !mLocalBounds.valid())
        {
            traverse(node, cv);
            return;
        }

        osg::Matrixd localViewInverse;
        const osg::Matrixd* viewInverse = nullptr;
        if (mCuller->hasCachedViewInverse())
            viewInverse = &mCuller->getCachedViewInverse();
        else
        {
            localViewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            viewInverse = &localViewInverse;
        }
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * (*viewInverse);
        const osg::BoundingBox worldBounds = transformLocalBounds(mLocalBounds, modelToWorld);
        const auto category = mGroundcover ? SceneUtil::OcclusionTestCategory::GroundcoverChunk
                                           : SceneUtil::OcclusionTestCategory::PagedChunk;
        if (mCuller->testVisibleCoarseAABB(worldBounds, category, mEstimatedChildren))
            traverse(node, cv);
    }

    CellOcclusionCallback::CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius,
        float occluderMaxRadius, float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage)
        : mCuller(culler)
        , mOccluderMinRadius(occluderMinRadius)
        , mOccluderMaxRadius(occluderMaxRadius)
        , mOccluderShrinkFactor(occluderShrinkFactor)
        , mOccluderMeshResolution(occluderMeshResolution)
        , mOccluderMaxMeshResolution(occluderMaxMeshResolution)
        , mOccluderInsideThreshold(occluderInsideThreshold)
        , mOccluderMaxDistanceSq(occluderMaxDistance * occluderMaxDistance)
        , mEnableStaticOccluders(enableStaticOccluders)
        , mV34BroadenOcclusion(v34BroadenOcclusion)
        , mV322Cp2OccluderMode(v322Cp2OccluderMode())
        , mMaxTriangles(maxTriangles)
        , mStorage(storage)
    {
    }

    const OccluderMesh& CellOcclusionCallback::getOccluderMesh(osg::Node* node)
    {
        auto it = mMeshCache.find(node);
        if (it != mMeshCache.end())
            return it->second;

        int meshRes = mOccluderMeshResolution;
        float radius = node->getBound().radius();
        // Modes 3-4 lower eligibility to 300 but deliberately keep the final V3.21
        // 400-unit radius as the proxy-detail reference. This adds smaller useful
        // occluders without making the existing 400+ population more expensive.
        const float detailReferenceRadius
            = mV322Cp2OccluderMode >= 3 ? 400.0f : mOccluderMinRadius;
        if (radius > detailReferenceRadius && detailReferenceRadius > 0)
        {
            float scale = radius / detailReferenceRadius;
            meshRes = std::clamp(
                static_cast<int>(mOccluderMeshResolution * scale), mOccluderMeshResolution, mOccluderMaxMeshResolution);
        }

        OccluderMesh mesh;
        const std::string_view modelPath = getModelPathForNode(node);
        if (mStorage && mStorage->isOpen() && !modelPath.empty())
        {
            OccluderMesh localMesh;
            if (mStorage->get(modelPath, meshRes, OcclusionStorage::makeShrinkKey(mOccluderShrinkFactor), localMesh))
            {
                const auto nodePaths = node->getParentalNodePaths();
                osg::Matrixf localToWorld;
                if (!nodePaths.empty())
                    localToWorld = osg::computeLocalToWorld(nodePaths.front());
                mesh = transformLocalMesh(localMesh, localToWorld);
            }
        }

        if (!mesh.aabb.valid() && mesh.vertices.empty() && mesh.indices.empty())
        {
            if (mStorage)
                mStorage->recordMiss();
            mesh = OcclusionCulling::buildSimplifiedMesh(node, meshRes, mOccluderShrinkFactor);
            // Persist to SQLite so future sessions skip buildSimplifiedMesh entirely.
            if (mStorage && mStorage->isOpen() && !modelPath.empty())
                mStorage->put(modelPath, meshRes, OcclusionStorage::makeShrinkKey(mOccluderShrinkFactor), mesh);
        }

        return mMeshCache.emplace(node, std::move(mesh)).first->second;
    }

    void CellOcclusionCallback::operator()(osg::Group* node, osgUtil::CullVisitor* cv)
    {
        // If occlusion is not active this frame (interior, shadow camera, etc.), traverse normally
        if (!mCuller->isFrameActive())
        {
            traverse(node, cv);
            return;
        }

        // Test cell bounding box against terrain-only depth — if fully hidden by terrain,
        // skip entire cell. Use terrain-only so buildings in adjacent cells don't
        // false-cull entire cells that are clearly in view.
        const osg::BoundingSphere& cellBS = node->getBound();
        if (cellBS.valid())
        {
            osg::BoundingBox cellBB;
            cellBB.expandBy(cellBS);

            if (!mCuller->testVisibleAABBTerrainOnly(cellBB))
                return; // Entire cell hidden by terrain — no children traversed
        }

        const unsigned int numChildren = node->getNumChildren();

        struct V322OccluderCandidate
        {
            const OccluderMesh* mesh = nullptr;
            float distanceSq = 0.0f;
            float utility = 0.0f;
            unsigned int triangles = 0;
        };
        std::vector<V322OccluderCandidate> v322Candidates;
        if (mV322Cp2OccluderMode > 0)
            v322Candidates.reserve(numChildren);

        // Pass 1: Large objects — test against terrain depth, optionally rasterize as occluders
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid() || bs.radius() < mOccluderMinRadius)
                continue;

            // Paged chunks and other oversized objects — test visibility, rasterize stored occluders
            if (bs.radius() > mOccluderMaxRadius)
            {
                // Rasterize sub-object occluder meshes stored at chunk creation time
                if (mEnableStaticOccluders)
                {
                    if (auto* udc = child->getUserDataContainer())
                    {
                        for (unsigned int j = 0; j < udc->getNumUserObjects(); ++j)
                        {
                            if (auto* pod = dynamic_cast<PagedOccluderData*>(udc->getUserObject(j)))
                            {
                                for (const auto& occMesh : pod->mOccluderMeshes)
                                {
                                    if (occMesh.indices.empty())
                                        continue;

                                    unsigned int newTris = static_cast<unsigned int>(occMesh.indices.size() / 3);
                                    if (mMaxTriangles > 0
                                        && mCuller->getNumBuildingTris() + newTris > mMaxTriangles)
                                        continue;

                                    mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);
                                    mCuller->incrementBuildingOccluders(
                                        newTris, static_cast<unsigned int>(occMesh.vertices.size()));
                                }
                                break; // Only one PagedOccluderData per chunk
                            }
                        }
                    }
                }

                // Test chunk visibility against terrain-only depth — paged chunks are large
                // geometry that should only be culled by terrain, not adjacent buildings.
                osg::BoundingBox pageBB;
                pageBB.expandBy(bs);
                if (mCuller->testVisibleAABBTerrainOnly(pageBB))
                    child->accept(*cv);
                continue;
            }

            // Get cached occluder mesh (with AABB for visibility test)
            const OccluderMesh& mesh = getOccluderMesh(child);

            // Rasterize as occluder if in range and camera is not inside the building.
            // Test against terrain-only buffer so other buildings don't prevent rasterization
            // of adjacent buildings (which would reduce culling coverage for Pass 2).
            bool v34RasterizedAsOccluder = false;
            if (mesh.aabb.valid() && mEnableStaticOccluders && !mesh.indices.empty()
                && mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
            {
                const float distSq = (bs.center() - cv->getEyePoint()).length2();
                if (distSq < mOccluderMaxDistanceSq)
                {
                    osg::Vec3f center = mesh.aabb.center();
                    osg::Vec3f halfExtent
                        = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                        * mOccluderInsideThreshold;
                    osg::BoundingBox scaledBB;
                    scaledBB.expandBy(center - halfExtent);
                    scaledBB.expandBy(center + halfExtent);
                    if (!scaledBB.contains(cv->getEyePoint()))
                    {
                        const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
                        if (mV322Cp2OccluderMode > 0)
                        {
                            const float radius = bs.radius();
                            const float radiusSq = std::max(radius * radius, 1.0f);
                            const float projectedUtility = radiusSq / std::max(distSq, radiusSq);
                            const float trianglePenalty = 1.0f + static_cast<float>(newTris) / 256.0f;
                            v322Candidates.push_back(
                                V322OccluderCandidate{ &mesh, distSq, projectedUtility / trianglePenalty, newTris });
                            // Treat a queued candidate as self-occlusion-sensitive even if the
                            // later ranked budget cannot fit it. CP2 never culls the building itself.
                            v34RasterizedAsOccluder = true;
                        }
                        else if (mMaxTriangles == 0
                            || mCuller->getNumBuildingTris() + newTris <= mMaxTriangles)
                        {
                            mCuller->rasterizeOccluder(mesh.vertices, mesh.indices);
                            mCuller->incrementBuildingOccluders(
                                newTris, static_cast<unsigned int>(mesh.vertices.size()));
                            v34RasterizedAsOccluder = true;
                        }
                    }
                }
            }

            // Keep the V3.3 anti-self-occlusion rule for admitted building proxies. V3.4 only expands full-buffer
            // testing to large objects that did NOT insert their own proxy this frame (out of range, over budget,
            // unsuitable mesh, or camera-inside exclusion), so no object is tested against itself.
            if (mV34BroadenOcclusion && !v34RasterizedAsOccluder)
            {
                osg::BoundingBox largeBB;
                largeBB.expandBy(bs);
                if (!mCuller->testVisibleAABB(largeBB))
                    continue;
            }
            child->accept(*cv);
        }

        if (mV322Cp2OccluderMode > 0 && !v322Candidates.empty())
        {
            if (mV322Cp2OccluderMode == 1)
            {
                // Mode 1: pure front-to-back budget consumption. This improves depth
                // usefulness without changing the eligibility population.
                std::sort(v322Candidates.begin(), v322Candidates.end(),
                    [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                        return lhs.distanceSq < rhs.distanceSq;
                    });
            }
            else
            {
                // Modes 2-4: approximate projected coverage per raster triangle.
                // Ties favor the nearer proxy so useful near depth arrives first.
                std::sort(v322Candidates.begin(), v322Candidates.end(),
                    [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                        if (lhs.utility != rhs.utility)
                            return lhs.utility > rhs.utility;
                        return lhs.distanceSq < rhs.distanceSq;
                    });
            }

            for (const auto& candidate : v322Candidates)
            {
                if (!candidate.mesh)
                    continue;
                if (mMaxTriangles > 0
                    && mCuller->getNumBuildingTris() + candidate.triangles > mMaxTriangles)
                    continue;

                // Mode 4 is intentionally aggressive but visibility-safe: if terrain or
                // an earlier ranked proxy already fully hides this candidate AABB, omit
                // only its redundant raster work. The building's scene traversal already
                // happened above and is never culled by this decision.
                if (mV322Cp2OccluderMode >= 4 && !mCuller->testVisibleAABB(candidate.mesh->aabb))
                    continue;

                mCuller->rasterizeOccluder(candidate.mesh->vertices, candidate.mesh->indices);
                mCuller->incrementBuildingOccluders(
                    candidate.triangles, static_cast<unsigned int>(candidate.mesh->vertices.size()));
            }
        }

        // Modes 3-4 decouple 300-unit occluder eligibility from the proven
        // 400-unit visibility-classification boundary. First record visibility for
        // every [300, 400) owner against the same terrain + 400+ proxy buffer that
        // the control path would use. No owner is traversed here: Pass 2 retains the
        // original child traversal order and consumes the recorded result later.
        std::vector<unsigned char> v322MidVisibility;
        if (mV322Cp2OccluderMode >= 3)
        {
            constexpr float v322EligibilityRadius = 300.0f;
            v322MidVisibility.assign(numChildren, 0);
            std::vector<V322OccluderCandidate> v322MidCandidates;
            v322MidCandidates.reserve(numChildren);

            for (unsigned int i = 0; i < numChildren; ++i)
            {
                osg::Node* child = node->getChild(i);
                const osg::BoundingSphere& bs = child->getBound();
                if (!bs.valid() || bs.radius() < v322EligibilityRadius || bs.radius() >= mOccluderMinRadius)
                    continue;

                bool skipOcclusion = false;
                child->getUserValue("skipOcclusion", skipOcclusion);

                osg::BoundingBox childBB;
                childBB.expandBy(bs);
                if (!skipOcclusion && !mCuller->testVisibleAABB(childBB))
                    continue;

                // Store the control-equivalent owner result before this object can
                // contribute any depth itself. Doors and other explicit exclusions
                // remain visible but are never admitted as CP2 occluders.
                v322MidVisibility[i] = 1;
                if (skipOcclusion || !mEnableStaticOccluders)
                    continue;

                const OccluderMesh& mesh = getOccluderMesh(child);
                if (!mesh.aabb.valid() || mesh.indices.empty() || !mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
                    continue;

                const float distSq = (bs.center() - cv->getEyePoint()).length2();
                if (distSq >= mOccluderMaxDistanceSq)
                    continue;

                osg::Vec3f center = mesh.aabb.center();
                osg::Vec3f halfExtent
                    = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                    * mOccluderInsideThreshold;
                osg::BoundingBox scaledBB;
                scaledBB.expandBy(center - halfExtent);
                scaledBB.expandBy(center + halfExtent);
                if (scaledBB.contains(cv->getEyePoint()))
                    continue;

                const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
                const float radius = bs.radius();
                const float radiusSq = std::max(radius * radius, 1.0f);
                const float projectedUtility = radiusSq / std::max(distSq, radiusSq);
                const float trianglePenalty = 1.0f + static_cast<float>(newTris) / 256.0f;
                v322MidCandidates.push_back(
                    V322OccluderCandidate{ &mesh, distSq, projectedUtility / trianglePenalty, newTris });
            }

            std::sort(v322MidCandidates.begin(), v322MidCandidates.end(),
                [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                    if (lhs.utility != rhs.utility)
                        return lhs.utility > rhs.utility;
                    return lhs.distanceSq < rhs.distanceSq;
                });

            for (const auto& candidate : v322MidCandidates)
            {
                if (!candidate.mesh)
                    continue;
                if (mMaxTriangles > 0
                    && mCuller->getNumBuildingTris() + candidate.triangles > mMaxTriangles)
                    continue;

                // Mode 4 may suppress only redundant proxy raster work. Owner visibility
                // was already resolved above without this proxy in depth, and actual owner
                // traversal remains deferred to Pass 2 in original child order.
                if (mV322Cp2OccluderMode >= 4 && !mCuller->testVisibleAABB(candidate.mesh->aabb))
                    continue;

                mCuller->rasterizeOccluder(candidate.mesh->vertices, candidate.mesh->indices);
                mCuller->incrementBuildingOccluders(
                    candidate.triangles, static_cast<unsigned int>(candidate.mesh->vertices.size()));
            }
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)
        for (unsigned int i = 0; i < numChildren; ++i)
        {
            osg::Node* child = node->getChild(i);
            const osg::BoundingSphere& bs = child->getBound();

            if (!bs.valid())
            {
                child->accept(*cv);
                continue;
            }

            if (bs.radius() >= mOccluderMinRadius)
                continue; // Already handled in pass 1

            if (mV322Cp2OccluderMode >= 3 && bs.radius() >= 300.0f)
            {
                // The visibility decision was recorded before any [300,400) proxy
                // entered the buffer, preventing self-occlusion while preserving
                // this original Pass-2 traversal position.
                if (i < v322MidVisibility.size() && v322MidVisibility[i] != 0)
                    child->accept(*cv);
                continue;
            }

            // Never occlude doors — they sit flush against building surfaces
            // and are easily falsely hidden by the parent building's AABB occluder
            bool skipOcclusion = false;
            child->getUserValue("skipOcclusion", skipOcclusion);

            osg::BoundingBox childBB;
            childBB.expandBy(bs);

            if (skipOcclusion || mCuller->testVisibleAABB(childBB))
                child->accept(*cv);
            // else: occluded — skip
        }
    }
}
