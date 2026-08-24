from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one match, found {count}\n--- needle ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# objectpaging.hpp: carry the shared OcclusionStorage pointer.
replace_once(
    "apps/openmw/mwrender/objectpaging.hpp",
    "namespace SceneUtil\n{\n    class OcclusionCuller;\n}\n",
    "namespace SceneUtil\n{\n    class OcclusionCuller;\n}\n\nnamespace OcclusionCulling\n{\n    class OcclusionStorage;\n}\n",
)
replace_once(
    "apps/openmw/mwrender/objectpaging.hpp",
    "        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles);\n",
    "        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,\n            OcclusionCulling::OcclusionStorage* storage);\n",
)
replace_once(
    "apps/openmw/mwrender/objectpaging.hpp",
    "        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;\n        unsigned int mMaxTriangles = 30000;\n",
    "        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;\n        unsigned int mMaxTriangles = 30000;\n        OcclusionCulling::OcclusionStorage* mOcclusionStorage = nullptr;\n",
)

# objectpaging.cpp: setter, model key tracking, and cached local-space proxy generation.
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    "    void ObjectPaging::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles)\n    {\n        mOcclusionCuller = culler;\n        mMaxTriangles = maxTriangles;\n    }\n",
    "    void ObjectPaging::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,\n        OcclusionCulling::OcclusionStorage* storage)\n    {\n        mOcclusionCuller = culler;\n        mMaxTriangles = maxTriangles;\n        mOcclusionStorage = storage;\n    }\n",
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    "        struct InstanceList\n        {\n            std::vector<const PagedCellRef*> mInstances;\n            AnalyzeVisitor::Result mAnalyzeResult;\n            bool mNeedCompile = false;\n        };\n",
    "        struct InstanceList\n        {\n            std::vector<const PagedCellRef*> mInstances;\n            AnalyzeVisitor::Result mAnalyzeResult;\n            VFS::Path::Normalized mModel;\n            bool mNeedCompile = false;\n        };\n",
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    "                emplaced.first->second.mAnalyzeResult = analyzeVisitor.retrieveResult();\n                emplaced.first->second.mNeedCompile = compile && nodePtr->referenceCount() <= 2;\n",
    "                emplaced.first->second.mAnalyzeResult = analyzeVisitor.retrieveResult();\n                emplaced.first->second.mModel = model;\n                emplaced.first->second.mNeedCompile = compile && nodePtr->referenceCount() <= 2;\n",
)

old_block = '''                // Build occluder mesh for building-sized objects
                if (buildOccluders)
                {
                    float scaledRadius = cnode->getBound().radius() * ref.mScale;
                    if (scaledRadius >= occluderMinRadius)
                    {
                        // Scale grid resolution with object size so grid cell size stays ~constant.
                        // A small building (radius 300) uses base resolution, a canton (radius 3000+)
                        // gets proportionally higher resolution to preserve shape detail.
                        int adaptiveRes = occluderMeshRes;
                        if (scaledRadius > occluderMinRadius)
                        {
                            float scale = scaledRadius / occluderMinRadius;
                            adaptiveRes = std::clamp(
                                static_cast<int>(occluderMeshRes * scale), occluderMeshRes, occluderMaxMeshRes);
                        }
                        auto occMesh = buildSimplifiedMesh(trans, adaptiveRes, occluderShrinkFactor);
                        if (!occMesh.indices.empty())
                        {
                            // Offset from chunk-relative to world-space
                            for (auto& v : occMesh.vertices)
                                v += worldCenter;
                            occMesh.aabb = osg::BoundingBox();
                            for (const auto& v : occMesh.vertices)
                                occMesh.aabb.expandBy(v);
                            pagedOccluderData->mOccluderMeshes.push_back(std::move(occMesh));
                        }
                    }
                }
'''
new_block = '''                // Build occluder mesh for building-sized objects. Cache the simplified proxy
                // in model-local space so repeated instances/chunks and future sessions can reuse it.
                if (buildOccluders)
                {
                    float scaledRadius = cnode->getBound().radius() * ref.mScale;
                    if (scaledRadius >= occluderMinRadius)
                    {
                        // Scale grid resolution with object size so grid cell size stays ~constant.
                        // A small building uses base resolution; very large structures get more detail.
                        int adaptiveRes = occluderMeshRes;
                        if (scaledRadius > occluderMinRadius)
                        {
                            float scale = scaledRadius / occluderMinRadius;
                            adaptiveRes = std::clamp(
                                static_cast<int>(occluderMeshRes * scale), occluderMeshRes, occluderMaxMeshRes);
                        }

                        const int shrinkKey = OcclusionStorage::makeShrinkKey(occluderShrinkFactor);
                        const int scaleKey = static_cast<int>(ref.mScale * 1000.f + (ref.mScale >= 0.f ? 0.5f : -0.5f));
                        std::string cacheKey = pair.second.mModel.value();
                        cacheKey += "|paged|";
                        cacheKey += activeGrid ? "active" : "distant";
                        cacheKey += "|lod=" + std::to_string(static_cast<unsigned int>(lod));
                        cacheKey += "|scale=" + std::to_string(scaleKey);

                        OccluderMesh localMesh;
                        bool cacheHit = false;
                        if (mOcclusionStorage && mOcclusionStorage->isOpen() && !cacheKey.empty())
                            cacheHit = mOcclusionStorage->get(cacheKey, adaptiveRes, shrinkKey, localMesh);

                        if (!cacheHit)
                        {
                            if (mOcclusionStorage && mOcclusionStorage->isOpen())
                                mOcclusionStorage->recordMiss();

                            // Reproduce the same CopyOp filtering/LOD selection as the rendered paged object,
                            // but omit this particular instance transform so the result is reusable.
                            osg::ref_ptr<osg::Group> localProxySource = new osg::Group;
                            copyop.copy(cnode, localProxySource);
                            localMesh = buildSimplifiedMesh(localProxySource, adaptiveRes, occluderShrinkFactor);

                            if (mOcclusionStorage && mOcclusionStorage->isOpen() && !cacheKey.empty())
                                mOcclusionStorage->put(cacheKey, adaptiveRes, shrinkKey, localMesh);
                        }

                        if (!localMesh.indices.empty())
                        {
                            // Apply this reference's transform to the cached local-space proxy, then
                            // offset the chunk-relative position into world space.
                            osg::Matrixf localToChunk = osg::Matrixf::identity();
                            localToChunk.preMultTranslate(nodePos);
                            localToChunk.preMultRotate(nodeAttitude);
                            localToChunk.preMultScale(nodeScale);

                            OccluderMesh occMesh;
                            occMesh.indices = localMesh.indices;
                            occMesh.vertices.reserve(localMesh.vertices.size());
                            for (const auto& v : localMesh.vertices)
                            {
                                osg::Vec3f worldVertex = v * localToChunk;
                                worldVertex += worldCenter;
                                occMesh.vertices.push_back(worldVertex);
                                occMesh.aabb.expandBy(worldVertex);
                            }
                            pagedOccluderData->mOccluderMeshes.push_back(std::move(occMesh));
                        }
                    }
                }
'''
replace_once("apps/openmw/mwrender/objectpaging.cpp", old_block, new_block)

# renderingmanager.cpp: pass storage to the default paging manager.
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "                mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles);\n",
    "                mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles, mOcclusionStorage.get());\n",
)

# New worldspaces created later must also inherit the active culler/cache.
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "                newChunkMgr.mObjectPaging\n                    = std::make_unique<ObjectPaging>(mResourceSystem->getSceneManager(), worldspace);\n                quadTreeWorld->addChunkManager(newChunkMgr.mObjectPaging.get());\n",
    "                newChunkMgr.mObjectPaging\n                    = std::make_unique<ObjectPaging>(mResourceSystem->getSceneManager(), worldspace);\n                if (mOcclusionCuller)\n                {\n                    const unsigned int maxTriangles\n                        = static_cast<unsigned int>(Settings::camera().mOcclusionMaxTriangles);\n                    newChunkMgr.mObjectPaging->setOcclusionCuller(\n                        mOcclusionCuller, maxTriangles, mOcclusionStorage.get());\n                }\n                quadTreeWorld->addChunkManager(newChunkMgr.mObjectPaging.get());\n",
)

# When the placeholder storage is replaced by the real DB, refresh all paging managers.
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "        if (mObjectPaging)\n            mObjectPaging->setOcclusionCuller(mOcclusionCuller, maxTriangles);\n        if (mSceneOcclusionCallback)\n            mSceneOcclusionCallback->setStorage(mOcclusionStorage.get());\n",
    "        for (auto& [worldspace, chunkMgr] : mWorldspaceChunks)\n        {\n            if (chunkMgr.mObjectPaging)\n                chunkMgr.mObjectPaging->setOcclusionCuller(\n                    mOcclusionCuller, maxTriangles, mOcclusionStorage.get());\n        }\n        if (mSceneOcclusionCallback)\n            mSceneOcclusionCallback->setStorage(mOcclusionStorage.get());\n",
)

# worldimp.cpp: actually open the persistent DB before cells/chunks begin loading.
replace_once(
    "apps/openmw/mwworld/worldimp.cpp",
    "        mRendering = std::make_unique<MWRender::RenderingManager>(\n            viewer, rootNode, mResourceSystem, workQueue, *mNavigator, mGroundcoverStore, unrefQueue);\n        mProjectileManager = std::make_unique<ProjectileManager>(\n",
    "        mRendering = std::make_unique<MWRender::RenderingManager>(\n            viewer, rootNode, mResourceSystem, workQueue, *mNavigator, mGroundcoverStore, unrefQueue);\n        if (Settings::camera().mOcclusionCulling)\n        {\n            const std::filesystem::path cachePath = mUserDataPath / \"occlusion-mesh-cache.sqlite\";\n            mRendering->setOcclusionCachePath(Files::pathToUnicodeString(cachePath));\n        }\n        mProjectileManager = std::make_unique<ProjectileManager>(\n",
)

print("Paged occluder cache patch applied successfully")
