#include "objects.hpp"

#include <osg/Group>
#include <osg/UserDataContainer>

#include <components/esm3/loaddoor.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/debug/v32rendererprofiling.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/unrefqueue.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "occlusionculling.hpp"
#include "vismask.hpp"

namespace MWRender
{

    Objects::Objects(Resource::ResourceSystem* resourceSystem, const osg::ref_ptr<osg::Group>& rootNode,
        SceneUtil::UnrefQueue& unrefQueue)
        : mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mUnrefQueue(unrefQueue)
    {
    }

    Objects::~Objects()
    {
        mRestoredObjects.clear();
        mHibernatedCells.clear();
        mObjects.clear();

        for (CellMap::iterator iter = mCellSceneNodes.begin(); iter != mCellSceneNodes.end(); ++iter)
        {
            if (iter->second->getNumParents() != 0)
                iter->second->getParent(0)->removeChild(iter->second);
        }
        mCellSceneNodes.clear();
    }

    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        Debug::V32RendererProfiling::ScopedPhase v32AttachTimer(
            Debug::V32RendererProfiling::Phase::TransformAttach);
        assert(mObjects.find(ptr.mRef) == mObjects.end());

        osg::ref_ptr<osg::Group> cellnode;

        CellMap::iterator found = mCellSceneNodes.find(ptr.getCell());
        if (found == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            cellnode->setName("Cell Root");
            if (mOcclusionCuller)
                cellnode->addCullCallback(new CellOcclusionCallback(mOcclusionCuller, mOccluderMinRadius,
                    mOccluderMaxRadius, mOccluderShrinkFactor, mOccluderMeshResolution, mOccluderMaxMeshResolution,
                    mOccluderInsideThreshold, mOccluderMaxDistance, mEnableStaticOccluders,
                    mV34BroadenOcclusion, mMaxTriangles, mOcclusionStorage));
            mRootNode->addChild(cellnode);
            mCellSceneNodes[ptr.getCell()] = cellnode;
        }
        else
            cellnode = found->second;

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> insert(new SceneUtil::PositionAttitudeTransform);
        cellnode->addChild(insert);

        insert->getOrCreateUserDataContainer()->addUserObject(new PtrHolder(ptr));

        if (ptr.getType() == ESM::REC_DOOR || ptr.getType() == ESM::REC_DOOR4)
            insert->setUserValue("skipOcclusion", true);

        const float* f = ptr.getRefData().getPosition().pos;

        insert->setPosition(osg::Vec3(f[0], f[1], f[2]));

        const float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        insert->setScale(scaleVec);

        ptr.getRefData().setBaseNode(std::move(insert));
    }

    void Objects::insertModel(const MWWorld::Ptr& ptr, const std::string& mesh, bool allowLight)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Object);
        bool animated = ptr.getClass().useAnim();
        std::string animationMesh = mesh;
        if (animated && !mesh.empty())
        {
            animationMesh = Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
            if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
                animated = false;
        }

        osg::ref_ptr<ObjectAnimation> anim(
            new ObjectAnimation(ptr, animationMesh, mResourceSystem, animated, allowLight));

        mObjects.emplace(ptr.mRef, std::move(anim));
    }

    void Objects::insertCreature(const MWWorld::Ptr& ptr, const std::string& mesh, bool weaponsShields)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        bool animated = true;
        std::string animationMesh
            = Misc::ResourceHelpers::correctActorModelPath(VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
        if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
            animated = false;

        // CreatureAnimation
        osg::ref_ptr<Animation> anim;

        if (weaponsShields)
            anim = new CreatureWeaponAnimation(ptr, animationMesh, mResourceSystem, animated);
        else
            anim = new CreatureAnimation(ptr, animationMesh, mResourceSystem, animated);

        if (mObjects.emplace(ptr.mRef, anim).second)
            ptr.getClass().getContainerStore(ptr).setContListener(static_cast<ActorAnimation*>(anim.get()));
    }

    void Objects::insertNPC(const MWWorld::Ptr& ptr)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        if (ptr.getType() == ESM::REC_NPC_4)
        {
            osg::ref_ptr<ESM4NpcAnimation> anim(
                new ESM4NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));
            mObjects.emplace(ptr.mRef, anim);
        }
        else
        {
            osg::ref_ptr<NpcAnimation> anim(
                new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));

            if (mObjects.emplace(ptr.mRef, anim).second)
            {
                ptr.getClass().getInventoryStore(ptr).setInvListener(anim.get());
                ptr.getClass().getInventoryStore(ptr).setContListener(anim.get());
            }
        }
    }

    bool Objects::removeObject(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getRefData().getBaseNode())
            return true;

        const auto iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
        {
            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            mObjects.erase(iter);

            if (ptr.getClass().isActor())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);

                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            ptr.getRefData().getBaseNode()->getParent(0)->removeChild(ptr.getRefData().getBaseNode());

            ptr.getRefData().setBaseNode(nullptr);
            return true;
        }
        return false;
    }

    void Objects::removeCell(const MWWorld::CellStore* store)
    {
        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (ptr.getCell() == store)
            {
                if (ptr.getClass().isActor() && ptr.getRefData().getCustomData())
                {
                    if (ptr.getClass().hasInventoryStore(ptr))
                        ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                    ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
                }

                iter->second->removeFromScene();
                mUnrefQueue.push(std::move(iter->second));
                iter = mObjects.erase(iter);
            }
            else
                ++iter;
        }

        CellMap::iterator cell = mCellSceneNodes.find(store);
        if (cell != mCellSceneNodes.end())
        {
            cell->second->getParent(0)->removeChild(cell->second);
            mCellSceneNodes.erase(cell);
        }
    }

    std::size_t Objects::hibernateCell(const MWWorld::CellStore* store)
    {
        auto cellNode = mCellSceneNodes.find(store);
        if (cellNode == mCellSceneNodes.end())
            return 0;

        // A store can only exist in one retained generation. Drop a stale entry
        // defensively rather than allowing two owners for the same scene nodes.
        auto stale = mHibernatedCells.find(store);
        if (stale != mHibernatedCells.end())
        {
            for (auto& [ref, object] : stale->second.mObjects)
            {
                (void)ref;
                if (object.mAnimation)
                {
                    object.mAnimation->removeFromScene();
                    mUnrefQueue.push(std::move(object.mAnimation));
                }
            }
            mHibernatedCells.erase(stale);
        }

        HibernatedCell retained;
        retained.mRoot = cellNode->second;

        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (ptr.getCell() != store)
            {
                ++iter;
                continue;
            }

            const bool safeStatic = !ptr.getClass().isActor() && !ptr.getClass().useAnim()
                && ptr.getType() != ESM::REC_DOOR && ptr.getType() != ESM::REC_DOOR4
                && ptr.getRefData().getBaseNode() != nullptr;

            if (safeStatic)
            {
                HibernatedObject object;
                object.mAnimation = std::move(iter->second);
                object.mBaseNode = ptr.getRefData().getBaseNode();
                retained.mObjects.emplace(ptr.mRef, std::move(object));
                iter = mObjects.erase(iter);
                continue;
            }

            // Everything not covered by the deliberately narrow static safety
            // predicate follows the normal destruction path.
            if (ptr.getClass().isActor() && ptr.getRefData().getCustomData())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            osg::ref_ptr<SceneUtil::PositionAttitudeTransform> baseNode = ptr.getRefData().getBaseNode();
            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            iter = mObjects.erase(iter);
            if (baseNode && baseNode->getNumParents() != 0)
                baseNode->getParent(0)->removeChild(baseNode);
        }

        if (retained.mRoot && retained.mRoot->getNumParents() != 0)
            retained.mRoot->getParent(0)->removeChild(retained.mRoot);
        mCellSceneNodes.erase(cellNode);

        const std::size_t retainedCount = retained.mObjects.size();
        if (retainedCount != 0)
            mHibernatedCells.emplace(store, std::move(retained));
        return retainedCount;
    }

    std::size_t Objects::restoreHibernatedCell(const MWWorld::CellStore* store)
    {
        auto found = mHibernatedCells.find(store);
        if (found == mHibernatedCells.end())
            return 0;

        HibernatedCell retained = std::move(found->second);
        mHibernatedCells.erase(found);

        if (!retained.mRoot)
            return 0;

        mRootNode->addChild(retained.mRoot);
        mCellSceneNodes[store] = retained.mRoot;

        std::size_t restoredCount = 0;
        for (auto& [ref, object] : retained.mObjects)
        {
            if (!object.mAnimation || !object.mBaseNode)
                continue;

            MWWorld::Ptr ptr = object.mAnimation->getPtr();
            const bool valid = ptr.getCell() == store && ptr.mRef == ref && !ptr.mRef->isDeleted()
                && ptr.getRefData().isEnabled() && !ptr.getClass().isActor() && !ptr.getClass().useAnim()
                && ptr.getType() != ESM::REC_DOOR && ptr.getType() != ESM::REC_DOOR4
                && mObjects.find(ref) == mObjects.end();

            if (!valid)
            {
                if (object.mBaseNode->getNumParents() != 0)
                    object.mBaseNode->getParent(0)->removeChild(object.mBaseNode);
                object.mAnimation->removeFromScene();
                mUnrefQueue.push(std::move(object.mAnimation));
                continue;
            }

            const float* position = ptr.getRefData().getPosition().pos;
            object.mBaseNode->setPosition(osg::Vec3f(position[0], position[1], position[2]));
            float scale = ptr.getCellRef().getScale();
            osg::Vec3f scaleVec(scale, scale, scale);
            ptr.getClass().adjustScale(ptr, scaleVec, true);
            object.mBaseNode->setScale(scaleVec);

            ptr.getRefData().setBaseNode(object.mBaseNode);
            mObjects.emplace(ref, std::move(object.mAnimation));
            mRestoredObjects.insert(ref);
            ++restoredCount;
        }

        return restoredCount;
    }

    bool Objects::consumeRestoredObject(const MWWorld::Ptr& ptr)
    {
        const auto found = mRestoredObjects.find(ptr.mRef);
        if (found == mRestoredObjects.end())
            return false;
        mRestoredObjects.erase(found);
        return true;
    }

    void Objects::clearHibernatedCells()
    {
        for (auto& [store, cell] : mHibernatedCells)
        {
            (void)store;
            for (auto& [ref, object] : cell.mObjects)
            {
                (void)ref;
                if (object.mAnimation)
                {
                    object.mAnimation->removeFromScene();
                    mUnrefQueue.push(std::move(object.mAnimation));
                }
            }
        }
        mHibernatedCells.clear();
        // Restored refs are normally consumed by Scene::addObject. Clear any
        // remainder at the restore boundary so an exception or a skipped ref
        // cannot make a later unrelated insertion look restored.
        mRestoredObjects.clear();
    }

    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {
        osg::ref_ptr<osg::Node> objectNode = cur.getRefData().getBaseNode();
        if (!objectNode)
            return;

        MWWorld::CellStore* newCell = cur.getCell();

        osg::Group* cellnode;
        if (mCellSceneNodes.find(newCell) == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            if (mOcclusionCuller)
                cellnode->addCullCallback(new CellOcclusionCallback(mOcclusionCuller, mOccluderMinRadius,
                    mOccluderMaxRadius, mOccluderShrinkFactor, mOccluderMeshResolution, mOccluderMaxMeshResolution,
                    mOccluderInsideThreshold, mOccluderMaxDistance, mEnableStaticOccluders,
                    mV34BroadenOcclusion, mMaxTriangles, mOcclusionStorage));
            mRootNode->addChild(cellnode);
            mCellSceneNodes[newCell] = cellnode;
        }
        else
        {
            cellnode = mCellSceneNodes[newCell];
        }

        osg::UserDataContainer* userDataContainer = objectNode->getUserDataContainer();
        if (userDataContainer)
            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    userDataContainer->setUserObject(i, new PtrHolder(cur));
            }

        if (objectNode->getNumParents())
            objectNode->getParent(0)->removeChild(objectNode);
        cellnode->addChild(objectNode);

        PtrAnimationMap::iterator iter = mObjects.find(old.mRef);
        if (iter != mObjects.end())
            iter->second->updatePtr(cur);
    }

    Animation* Objects::getAnimation(const MWWorld::Ptr& ptr)
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

    const Animation* Objects::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

    void Objects::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, float occluderMinRadius,
        float occluderMaxRadius, float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage)
    {
        mOcclusionCuller = culler;
        mOccluderMinRadius = occluderMinRadius;
        mOccluderMaxRadius = occluderMaxRadius;
        mOccluderShrinkFactor = occluderShrinkFactor;
        mOccluderMeshResolution = occluderMeshResolution;
        mOccluderMaxMeshResolution = occluderMaxMeshResolution;
        mOccluderInsideThreshold = occluderInsideThreshold;
        mOccluderMaxDistance = occluderMaxDistance;
        mEnableStaticOccluders = enableStaticOccluders;
        mV34BroadenOcclusion = v34BroadenOcclusion;
        mMaxTriangles = maxTriangles;
        mOcclusionStorage = storage;
    }

}
