from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.2 hibernation match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.2 hibernation patched {rel}")


# Keep the retained render state owned by MWRender::Objects, which already owns
# both the per-cell scene roots and the Animation instances. Scene only decides
# when a whole exterior grid is eligible for hibernation.
replace_once(
    "apps/openmw/mwrender/objects.hpp",
    '''#include <map>
#include <string>''',
    '''#include <cstddef>
#include <map>
#include <set>
#include <string>''',
)

replace_once(
    "apps/openmw/mwrender/objects.hpp",
    '''namespace SceneUtil
{
    class OcclusionCuller;
    class UnrefQueue;
}''',
    '''namespace SceneUtil
{
    class OcclusionCuller;
    class PositionAttitudeTransform;
    class UnrefQueue;
}''',
)

replace_once(
    "apps/openmw/mwrender/objects.hpp",
    '''        typedef std::map<const MWWorld::CellStore*, osg::ref_ptr<osg::Group>> CellMap;
        CellMap mCellSceneNodes;
        PtrAnimationMap mObjects;
        osg::ref_ptr<osg::Group> mRootNode;''',
    '''        typedef std::map<const MWWorld::CellStore*, osg::ref_ptr<osg::Group>> CellMap;

        struct HibernatedObject
        {
            osg::ref_ptr<Animation> mAnimation;
            osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mBaseNode;
        };
        using HibernatedObjectMap = std::map<const MWWorld::LiveCellRefBase*, HibernatedObject>;
        struct HibernatedCell
        {
            osg::ref_ptr<osg::Group> mRoot;
            HibernatedObjectMap mObjects;
        };
        using HibernatedCellMap = std::map<const MWWorld::CellStore*, HibernatedCell>;

        CellMap mCellSceneNodes;
        PtrAnimationMap mObjects;
        HibernatedCellMap mHibernatedCells;
        std::set<const MWWorld::LiveCellRefBase*> mRestoredObjects;
        osg::ref_ptr<osg::Group> mRootNode;''',
)

replace_once(
    "apps/openmw/mwrender/objects.hpp",
    '''        void removeCell(const MWWorld::CellStore* store);

        /// Updates containing cell for object rendering data''',
    '''        void removeCell(const MWWorld::CellStore* store);

        // V3.2: preserve only renderer-safe static state. Actors, doors and
        // objects using the animation path continue through normal destruction.
        std::size_t hibernateCell(const MWWorld::CellStore* store);
        std::size_t restoreHibernatedCell(const MWWorld::CellStore* store);
        bool consumeRestoredObject(const MWWorld::Ptr& ptr);
        void clearHibernatedCells();

        /// Updates containing cell for object rendering data''',
)

# Hibernated roots are detached from mRootNode and therefore need independent
# lifetime cleanup. Active objects retain the original destructor path.
replace_once(
    "apps/openmw/mwrender/objects.cpp",
    '''    Objects::~Objects()
    {
        mObjects.clear();

        for (CellMap::iterator iter = mCellSceneNodes.begin(); iter != mCellSceneNodes.end(); ++iter)
            iter->second->getParent(0)->removeChild(iter->second);
        mCellSceneNodes.clear();
    }''',
    '''    Objects::~Objects()
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
    }''',
)

# Insert hibernation implementation immediately after the proven removeCell
# implementation. Unsafe objects are destroyed normally before the cell root is
# detached; only non-actor, non-door, non-useAnim ObjectAnimations are retained.
replace_once(
    "apps/openmw/mwrender/objects.cpp",
    '''    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {''',
    '''    std::size_t Objects::hibernateCell(const MWWorld::CellStore* store)
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
    }

    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {''',
)

# RenderingManager owns the transition between active and detached Objects state.
replace_once(
    "apps/openmw/mwrender/renderingmanager.hpp",
    '''        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);''',
    '''        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);

        // V3.2 bounded recent-exterior render-state retention.
        bool beginExteriorHibernation();
        std::size_t hibernateExteriorCell(const MWWorld::CellStore* store);
        std::size_t restoreHibernatedExteriorCell(const MWWorld::CellStore* store);
        bool consumeRestoredExteriorObject(const MWWorld::Ptr& ptr);
        void finishExteriorRestore();''',
)

replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        mObjects->removeCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }

        mWater->removeCell(store);
    }

    void RenderingManager::enableTerrain''',
    '''    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        mObjects->removeCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }

        mWater->removeCell(store);
    }

    bool RenderingManager::beginExteriorHibernation()
    {
        // V1 intentionally retains one recent exterior grid only. Starting a
        // new exterior->interior transition always evicts the previous grid.
        mObjects->clearHibernatedCells();

        if (!Settings::cells().mV32ExteriorHibernation)
            return false;
        if (Settings::cells().mV32GpuMemoryManagement && Debug::V3GpuMemory::hardPressure())
            return false;
        return true;
    }

    std::size_t RenderingManager::hibernateExteriorCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        const std::size_t retained = mObjects->hibernateCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }
        mWater->removeCell(store);
        return retained;
    }

    std::size_t RenderingManager::restoreHibernatedExteriorCell(const MWWorld::CellStore* store)
    {
        if (!Settings::cells().mV32ExteriorHibernation)
            return 0;
        if (Settings::cells().mV32GpuMemoryManagement && Debug::V3GpuMemory::hardPressure())
        {
            mObjects->clearHibernatedCells();
            return 0;
        }
        return mObjects->restoreHibernatedCell(store);
    }

    bool RenderingManager::consumeRestoredExteriorObject(const MWWorld::Ptr& ptr)
    {
        return mObjects->consumeRestoredObject(ptr);
    }

    void RenderingManager::finishExteriorRestore()
    {
        // Any cells not consumed by the destination grid are stale. This is the
        // hard bound that prevents V1 from accumulating grids across travel.
        mObjects->clearHibernatedCells();
    }

    void RenderingManager::enableTerrain''',
)

replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::clear()
    {
        mSky->setMoonColour(false);

        notifyWorldSpaceChanged();''',
    '''    void RenderingManager::clear()
    {
        mSky->setMoonColour(false);
        mObjects->clearHibernatedCells();

        notifyWorldSpaceChanged();''',
)

# Scene gets a single explicit boolean for the transition. All pre-existing
# unload callers retain normal behavior through the default argument.
replace_once(
    "apps/openmw/mwworld/scene.hpp",
    '''        void unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard);''',
    '''        void unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard,
            bool hibernateRenderState = false);''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)''',
    '''    void Scene::unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard,
        bool hibernateRenderState)''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        ListAndResetObjectsVisitor visitor;

        cell->forEach(visitor, true); // Include objects being teleported by Lua''',
    '''        // Render state must be captured before ListAndResetObjectsVisitor
        // clears RefData::baseNode for every object in the cell.
        std::size_t v32HibernatedObjects = 0;
        if (hibernateRenderState)
            v32HibernatedObjects = mRendering.hibernateExteriorCell(cell);

        ListAndResetObjectsVisitor visitor;

        cell->forEach(visitor, true); // Include objects being teleported by Lua''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        mRendering.removeCell(cell);
        MWBase::Environment::get().getWindowManager()->removeCell(cell);''',
    '''        if (!hibernateRenderState)
            mRendering.removeCell(cell);
        else if (v32HibernatedObjects != 0)
            Log(Debug::Info) << "V3.2 hibernated " << v32HibernatedObjects << " static render objects from "
                             << cell->getCell()->getDescription();
        MWBase::Environment::get().getWindowManager()->removeCell(cell);''',
)

# Restore the detached cell root after respawn/local-script setup but before
# insertCell. insertCell will then rebuild physics/Lua/nav while skipping only
# the already-retained renderer construction for validated static refs.
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        if (respawn)
            cell.respawn();

        insertCell(cell, loadingListener, navigatorUpdateGuard);''',
    '''        if (respawn)
            cell.respawn();

        std::size_t v32RestoredObjects = 0;
        if (cellVariant.isExterior())
            v32RestoredObjects = mRendering.restoreHibernatedExteriorCell(&cell);
        if (v32RestoredObjects != 0)
            Log(Debug::Info) << "V3.2 restored " << v32RestoredObjects << " static render objects into "
                             << cell.getCell()->getDescription();

        insertCell(cell, loadingListener, navigatorUpdateGuard);''',
)

# Permit exactly the Objects entries marked by restoreHibernatedCell to bypass
# renderer construction. Physics, Lua scene registration and navigation are
# rebuilt through the normal path. If paging policy changed while indoors, drop
# the retained object and honor the new paged representation instead.
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
        {
            Log(Debug::Warning) << "Warning: Tried to add " << ptr.getCellRef().getRefId() << " to the scene twice";
            return;
        }

        const VFS::Path::Normalized model = getModel(ptr);
        const auto rotation = makeDirectNodeRotation(ptr);

        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (!refnum.hasContentFile() || !std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum))
            ptr.getClass().insertObjectRendering(ptr, model, rendering);
        else
            ptr.getRefData().setBaseNode(pagedNode);
        setNodeRotation(ptr, rendering, rotation);''',
    '''        bool restoredRendering = false;
        if (ptr.getRefData().getBaseNode())
            restoredRendering = rendering.consumeRestoredExteriorObject(ptr);
        if ((ptr.getRefData().getBaseNode() && !restoredRendering) || physics.getActor(ptr))
        {
            Log(Debug::Warning) << "Warning: Tried to add " << ptr.getCellRef().getRefId() << " to the scene twice";
            return;
        }

        const VFS::Path::Normalized model = getModel(ptr);
        const auto rotation = makeDirectNodeRotation(ptr);

        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        const bool shouldPage
            = refnum.hasContentFile() && std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum);
        if (restoredRendering && shouldPage)
        {
            // Paging can theoretically change while the player is indoors. Do
            // not keep both a retained normal object and a paged representation.
            rendering.removeObject(ptr);
            restoredRendering = false;
        }

        if (!restoredRendering)
        {
            if (!shouldPage)
                ptr.getClass().insertObjectRendering(ptr, model, rendering);
            else
                ptr.getRefData().setBaseNode(pagedNode);
        }
        setNodeRotation(ptr, rendering, rotation);''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        // Restore effect particles
        world.applyLoopingParticles(ptr);''',
    '''        // A retained ObjectAnimation already owns its particle/render
        // state. Re-applying looping particles would duplicate it.
        if (!restoredRendering)
            world.applyLoopingParticles(ptr);''',
)

# Only an exterior->interior transition can populate the V1 cache. Nested
# interior travel deliberately leaves the previous exterior cache untouched.
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();

        // unload
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)''',
    '''        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();

        const bool hibernateExterior = mCurrentCell != nullptr && mCurrentCell->isExterior()
            && mRendering.beginExteriorHibernation();

        // unload
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''            auto* cellToUnload = *iter++;
            unloadCell(cellToUnload, navigatorUpdateGuard.get());
        }
        assert(mActiveCells.empty());''',
    '''            auto* cellToUnload = *iter++;
            unloadCell(cellToUnload, navigatorUpdateGuard.get(), hibernateExterior && cellToUnload->isExterior());
        }
        assert(mActiveCells.empty());''',
)

# Once all destination cells have had a chance to restore, evict anything left
# over from a different grid/worldspace so V1 can never accumulate indefinitely.
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        mNavigator.update(pos, navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();''',
    '''        mRendering.finishExteriorRestore();
        mNavigator.update(pos, navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();''',
)

print("V3.2 bounded exterior hibernation patch completed successfully.")
