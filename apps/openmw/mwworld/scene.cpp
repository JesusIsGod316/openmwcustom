#include "scene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v32rendererprofiling.hpp>
#include <components/debug/v3gpumemory.hpp>
#include <components/detournavigator/agentbounds.hpp>
#include <components/detournavigator/debug.hpp>
#include <components/detournavigator/heightfieldshape.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/updateguard.hpp>
#include <components/esm/esmterrain.hpp>
#include <components/esm/records.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/terraingrid.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/landmanager.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwphysics/actor.hpp"
#include "../mwphysics/heightfield.hpp"
#include "../mwphysics/object.hpp"
#include "../mwphysics/physicssystem.hpp"

#include "../mwworld/actionteleport.hpp"

#include "cellpreloader.hpp"
#include "cellstore.hpp"
#include "cellvisitors.hpp"
#include "class.hpp"
#include "esmstore.hpp"
#include "localscripts.hpp"
#include "player.hpp"
#include "worldimp.hpp"

namespace
{
    using MWWorld::RotationOrder;

    osg::Quat makeActorOsgQuat(const ESM::Position& position)
    {
        return osg::Quat(position.rot[2], osg::Vec3(0, 0, -1));
    }

    osg::Quat makeInversedOrderObjectOsgQuat(const ESM::Position& position)
    {
        const float xr = position.rot[0];
        const float yr = position.rot[1];
        const float zr = position.rot[2];

        return osg::Quat(xr, osg::Vec3(-1, 0, 0)) * osg::Quat(yr, osg::Vec3(0, -1, 0))
            * osg::Quat(zr, osg::Vec3(0, 0, -1));
    }

    osg::Quat makeInverseNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? makeActorOsgQuat(pos) : makeInversedOrderObjectOsgQuat(pos);
    }

    osg::Quat makeDirectNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? makeActorOsgQuat(pos) : Misc::Convert::makeOsgQuat(pos);
    }

    osg::Quat makeNodeRotation(const MWWorld::Ptr& ptr, RotationOrder order)
    {
        if (order == RotationOrder::inverse)
            return makeInverseNodeRotation(ptr);
        return makeDirectNodeRotation(ptr);
    }

    void setNodeRotation(const MWWorld::Ptr& ptr, MWRender::RenderingManager& rendering, const osg::Quat& rotation)
    {
        if (ptr.getRefData().getBaseNode())
            rendering.rotateObject(ptr, rotation);
    }

    VFS::Path::Normalized getModel(const MWWorld::Ptr& ptr)
    {
        if (Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return {};
        return ptr.getClass().getCorrectedModel(ptr);
    }

    // Null node meant to distinguish objects that aren't in the scene from paged objects
    // TODO: find a more clever way to make paging exclusion more reliable?
    static osg::ref_ptr<SceneUtil::PositionAttitudeTransform> pagedNode = new SceneUtil::PositionAttitudeTransform;

    struct V3InsertionAccumulator
    {
        std::size_t mTotalRefs = 0;
        std::size_t mRenderedRefs = 0;
        std::size_t mPhysicsRefs = 0;
        std::size_t mActors = 0;
        std::size_t mAnimated = 0;
        std::size_t mDoors = 0;
        double mRenderMs = 0.0;
        double mMechanicsMs = 0.0;
        double mParticlesMs = 0.0;
        double mPhysicsMs = 0.0;
        double mLuaAddedMs = 0.0;
        double mNavMs = 0.0;
    };

    thread_local V3InsertionAccumulator* sV3InsertionAccumulator = nullptr;

    class V3InsertionAccumulatorScope
    {
    public:
        explicit V3InsertionAccumulatorScope(V3InsertionAccumulator* current)
            : mPrevious(sV3InsertionAccumulator)
        {
            sV3InsertionAccumulator = current;
        }

        ~V3InsertionAccumulatorScope() { sV3InsertionAccumulator = mPrevious; }

        V3InsertionAccumulatorScope(const V3InsertionAccumulatorScope&) = delete;
        V3InsertionAccumulatorScope& operator=(const V3InsertionAccumulatorScope&) = delete;

    private:
        V3InsertionAccumulator* mPrevious = nullptr;
    };

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const std::vector<ESM::RefNum>& pagedRefs,
        MWPhysics::PhysicsSystem& physics, MWRender::RenderingManager& rendering)
    {
        bool restoredRendering = false;
        if (ptr.getRefData().getBaseNode())
            restoredRendering = rendering.consumeRestoredExteriorObject(ptr);
        if ((ptr.getRefData().getBaseNode() && !restoredRendering) || physics.getActor(ptr))
        {
            Log(Debug::Warning) << "Warning: Tried to add " << ptr.getCellRef().getRefId() << " to the scene twice";
            return;
        }

        V3InsertionAccumulator* const stats = sV3InsertionAccumulator;
        if (stats)
        {
            ++stats->mTotalRefs;
            if (ptr.getClass().isActor())
                ++stats->mActors;
            if (ptr.getClass().useAnim())
                ++stats->mAnimated;
            if (ptr.getClass().isDoor())
                ++stats->mDoors;
        }

        const VFS::Path::Normalized model = getModel(ptr);
        const auto rotation = makeDirectNodeRotation(ptr);

        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        const bool paged = refnum.hasContentFile() && std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum);
        if (restoredRendering && paged)
        {
            // Paging policy may change while indoors. Never keep both forms.
            rendering.removeObject(ptr);
            restoredRendering = false;
        }
        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        const auto v32ObjectStart = Debug::V32RendererProfiling::beginObject();
        if (!restoredRendering)
        {
            if (!paged)
            {
                ptr.getClass().insertObjectRendering(ptr, model, rendering);
                if (stats)
                    ++stats->mRenderedRefs;
            }
            else
                ptr.getRefData().setBaseNode(pagedNode);
        }
        else if (stats)
            ++stats->mRenderedRefs;
        setNodeRotation(ptr, rendering, rotation);
        Debug::V32RendererProfiling::finishObject(v32ObjectStart, !restoredRendering && !paged, restoredRendering,
            paged, ptr.getClass().isActor(), ptr.getClass().useAnim(),
            ptr.getType() == ESM::Light::sRecordId || ptr.getType() == ESM4::Light::sRecordId,
            ptr.getCellRef().getRefId().toDebugString(), model.value());
        if (stats)
            stats->mRenderMs += Debug::V3Diagnostics::elapsedMs(phaseStart);

        phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (ptr.getClass().useAnim())
            MWBase::Environment::get().getMechanicsManager()->add(ptr);
        if (stats)
            stats->mMechanicsMs += Debug::V3Diagnostics::elapsedMs(phaseStart);

        phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (ptr.getClass().isActor())
            rendering.addWaterRippleEmitter(ptr);
        // A retained ObjectAnimation already owns its particle/render
        // state. Re-applying looping particles would duplicate it.
        if (!restoredRendering)
            world.applyLoopingParticles(ptr);
        if (stats)
            stats->mParticlesMs += Debug::V3Diagnostics::elapsedMs(phaseStart);

        phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (!model.empty())
        {
            ptr.getClass().insertObject(ptr, model, rotation, physics);
            if (stats)
                ++stats->mPhysicsRefs;
        }
        if (stats)
            stats->mPhysicsMs += Debug::V3Diagnostics::elapsedMs(phaseStart);

        phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        MWBase::Environment::get().getLuaManager()->objectAddedToScene(ptr);
        if (stats)
            stats->mLuaAddedMs += Debug::V3Diagnostics::elapsedMs(phaseStart);
    }

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const MWPhysics::PhysicsSystem& physics,
        float& lowestPoint, bool isInterior, DetourNavigator::Navigator& navigator,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard = nullptr)
    {
        V3InsertionAccumulator* const stats = sV3InsertionAccumulator;
        const auto navStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (const auto object = physics.getObject(ptr))
        {
            // Find the lowest point of this collision object in world space from its AABB if interior
            // this point is used to determine the infinite fall cutoff from lowest point in the cell
            if (isInterior)
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                const auto transform = object->getTransform();
                object->getShapeInstance()->mCollisionShape->getAabb(transform, aabbMin, aabbMax);
                lowestPoint = std::min(lowestPoint, static_cast<float>(aabbMin.z()));
            }

            const DetourNavigator::ObjectTransform objectTransform{ ptr.getRefData().getPosition(),
                ptr.getCellRef().getScale() };

            if (ptr.getClass().isDoor() && !ptr.getCellRef().getTeleport())
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                object->getShapeInstance()->mCollisionShape->getAabb(btTransform::getIdentity(), aabbMin, aabbMax);

                const auto center = (aabbMax + aabbMin) * 0.5f;

                const auto distanceFromDoor = world.getMaxActivationDistance() * 0.5f;
                const auto toPoint = aabbMax.x() - aabbMin.x() < aabbMax.y() - aabbMin.y()
                    ? btVector3(distanceFromDoor, 0, 0)
                    : btVector3(0, distanceFromDoor, 0);

                const auto transform = object->getTransform();
                const btTransform closedDoorTransform(
                    Misc::Convert::makeBulletQuaternion(ptr.getCellRef().getPosition()), transform.getOrigin());

                const auto start = Misc::Convert::makeOsgVec3f(closedDoorTransform(center + toPoint));
                const auto startPoint = physics.castRay(start, start - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionStart = startPoint.mHit ? startPoint.mHitPos : start;

                const auto end = Misc::Convert::makeOsgVec3f(closedDoorTransform(center - toPoint));
                const auto endPoint = physics.castRay(end, end - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionEnd = endPoint.mHit ? endPoint.mHitPos : end;

                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::DoorShapes(
                        object->getShapeInstance(), objectTransform, connectionStart, connectionEnd),
                    transform, navigatorUpdateGuard);
            }
            else if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
            {
                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::ObjectShapes(object->getShapeInstance(), objectTransform), object->getTransform(),
                    navigatorUpdateGuard);
            }
        }
        else if (physics.getActor(ptr))
        {
            const DetourNavigator::AgentBounds agentBounds = world.getPathfindingAgentBounds(ptr);
            if (!navigator.addAgent(agentBounds))
                Log(Debug::Warning) << "Agent bounds are not supported by navigator for " << ptr.toString() << ": "
                                    << agentBounds;
        }
        if (stats)
            stats->mNavMs += Debug::V3Diagnostics::elapsedMs(navStart);
    }

    struct InsertVisitor
    {
        MWWorld::CellStore& mCell;
        Loading::Listener* mLoadingListener;

        std::vector<MWWorld::Ptr> mToInsert;

        InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener);

        bool operator()(const MWWorld::Ptr& ptr);

        template <class AddObject>
        void insert(AddObject&& addObject);
    };

    InsertVisitor::InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener)
        : mCell(cell)
        , mLoadingListener(loadingListener)
    {
    }

    bool InsertVisitor::operator()(const MWWorld::Ptr& ptr)
    {
        // do not insert directly as we can't modify the cell from within the visitation
        // CreatureLevList::insertObjectRendering may spawn a new creature
        mToInsert.push_back(ptr);
        return true;
    }

    template <class AddObject>
    void InsertVisitor::insert(AddObject&& addObject)
    {
        for (MWWorld::Ptr& ptr : mToInsert)
        {
            if (!ptr.mRef->isDeleted() && ptr.getRefData().isEnabled())
            {
                try
                {
                    addObject(ptr);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
                }
            }

            if (mLoadingListener != nullptr)
                mLoadingListener->increaseProgress(1);
        }
    }

    int getCellPositionDistanceToOrigin(const std::pair<int, int>& cellPosition)
    {
        return std::abs(cellPosition.first) + std::abs(cellPosition.second);
    }

    bool isCellInCollection(ESM::ExteriorCellLocation cellIndex, MWWorld::Scene::CellStoreCollection& collection)
    {
        for (auto* cell : collection)
        {
            assert(cell->getCell()->isExterior());
            if (cellIndex == cell->getCell()->getExteriorCellLocation())
                return true;
        }
        return false;
    }

    bool removeFromSorted(ESM::RefNum refNum, std::vector<ESM::RefNum>& pagedRefs)
    {
        const auto it = std::lower_bound(pagedRefs.begin(), pagedRefs.end(), refNum);
        if (it == pagedRefs.end() || *it != refNum)
            return false;
        pagedRefs.erase(it);
        return true;
    }

    template <class Function>
    void iterateOverCellsAround(int cellX, int cellY, int range, Function&& f)
    {
        for (int x = cellX - range, lastX = cellX + range; x <= lastX; ++x)
            for (int y = cellY - range, lastY = cellY + range; y <= lastY; ++y)
                f(x, y);
    }

    void sortCellsToLoad(int centerX, int centerY, std::vector<std::pair<int, int>>& cells)
    {
        const auto getDistanceToPlayerCell = [&](const std::pair<int, int>& cellPosition) {
            return std::abs(cellPosition.first - centerX) + std::abs(cellPosition.second - centerY);
        };

        const auto getCellPositionPriority = [&](const std::pair<int, int>& cellPosition) {
            return std::make_pair(getDistanceToPlayerCell(cellPosition), getCellPositionDistanceToOrigin(cellPosition));
        };

        std::sort(cells.begin(), cells.end(), [&](const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) {
            return getCellPositionPriority(lhs) < getCellPositionPriority(rhs);
        });
    }
}

namespace MWWorld
{
    void Scene::removeFromPagedRefs(const Ptr& ptr)
    {
        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (refnum.hasContentFile() && removeFromSorted(refnum, mPagedRefs))
        {
            if (!ptr.getRefData().getBaseNode())
                return;
            ptr.getClass().insertObjectRendering(ptr, getModel(ptr), mRendering);
            setNodeRotation(ptr, mRendering, makeNodeRotation(ptr, RotationOrder::direct));
            reloadTerrain();
        }
    }

    bool Scene::isPagedRef(const Ptr& ptr) const
    {
        return ptr.getRefData().getBaseNode() == pagedNode.get();
    }

    void Scene::updateObjectRotation(const Ptr& ptr, RotationOrder order)
    {
        const auto rot = makeNodeRotation(ptr, order);
        setNodeRotation(ptr, mRendering, rot);
        mPhysics->updateRotation(ptr, rot);
    }

    void Scene::updateObjectScale(const Ptr& ptr)
    {
        float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        mRendering.scaleObject(ptr, scaleVec);
        mPhysics->updateScale(ptr);
    }

    void Scene::update(float duration)
    {
        if (mChangeCellGridRequest.has_value())
        {
            changeCellGrid(mChangeCellGridRequest->mPosition, mChangeCellGridRequest->mCellIndex,
                mChangeCellGridRequest->mChangeEvent);
            mChangeCellGridRequest.reset();
        }

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preloader_cache_update", "", 0.5);
            mPreloader->updateCache(mRendering.getReferenceTime());
        }
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preload_schedule", "", 0.5);
            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
            const float targetMs = Settings::RamCache::streamingTargetFrameMs();

            const bool adaptiveV1 = Settings::RamCache::adaptiveStreamingEnabled();
            const bool adaptiveV2 = Settings::RamCache::adaptiveStreamingV2Enabled();
            bool defer = false;
            bool forcedProgress = false;
            const char* eventDetail = "pressure";
            int deferLimit = 1;
            int deferCount = 1;

            // This state is main-thread-only: Scene::update owns predictive
            // preload scheduling. Reset it when v2 is not selected so changing
            // scheduler modes cannot inherit stale pressure or job age.
            static double v32SmoothedFrameMs = 0.0;
            static unsigned v32BadFrameStreak = 0;
            static unsigned v32RecoveryFrameStreak = 0;
            static unsigned v32ConsecutiveDefers = 0;
            static bool v32PressureMode = false;
            static bool v32WasActive = false;

            if (!adaptiveV2 && v32WasActive)
            {
                v32SmoothedFrameMs = 0.0;
                v32BadFrameStreak = 0;
                v32RecoveryFrameStreak = 0;
                v32ConsecutiveDefers = 0;
                v32PressureMode = false;
                v32WasActive = false;
            }

            if (adaptiveV1)
            {
                // Preserve the exact V1 experiment for historical A/B testing.
                const bool pressure = lastFrameMs > targetMs;
                defer = pressure && (frame & 1u);
            }
            else if (adaptiveV2)
            {
                v32WasActive = true;

                if (lastFrameMs > 0.0)
                {
                    constexpr double alpha = 0.20;
                    v32SmoothedFrameMs = v32SmoothedFrameMs <= 0.0
                        ? lastFrameMs
                        : (v32SmoothedFrameMs * (1.0 - alpha) + lastFrameMs * alpha);
                }

                const double enterPressureMs = static_cast<double>(targetMs) * 1.05;
                const double leavePressureMs = static_cast<double>(targetMs) * 0.92;
                constexpr unsigned enterPressureFrames = 3;
                constexpr unsigned leavePressureFrames = 6;

                if (!v32PressureMode)
                {
                    v32RecoveryFrameStreak = 0;
                    if (v32SmoothedFrameMs > enterPressureMs)
                        ++v32BadFrameStreak;
                    else
                        v32BadFrameStreak = 0;

                    if (v32BadFrameStreak >= enterPressureFrames)
                    {
                        v32PressureMode = true;
                        v32BadFrameStreak = 0;
                    }
                }
                else
                {
                    if (v32SmoothedFrameMs < leavePressureMs)
                        ++v32RecoveryFrameStreak;
                    else
                        v32RecoveryFrameStreak = 0;

                    if (v32RecoveryFrameStreak >= leavePressureFrames)
                    {
                        v32PressureMode = false;
                        v32RecoveryFrameStreak = 0;
                        v32ConsecutiveDefers = 0;
                    }
                }

                const unsigned maxDefers = static_cast<unsigned>(
                    std::max(Settings::RamCache::streamingMaxDefers(), 0));

                // Forced progress: after maxDefers consecutive skips, the next
                // opportunity always runs preloadCells even while under pressure.
                if (v32PressureMode && maxDefers != 0u)
                {
                    if (v32ConsecutiveDefers < maxDefers)
                    {
                        defer = true;
                        ++v32ConsecutiveDefers;
                    }
                    else
                    {
                        forcedProgress = true;
                        v32ConsecutiveDefers = 0;
                    }
                }
                else
                    v32ConsecutiveDefers = 0;

                eventDetail = forcedProgress ? "v2_forced_progress" : "v2_smoothed_pressure";
                deferLimit = static_cast<int>(maxDefers);
                deferCount = static_cast<int>(v32ConsecutiveDefers);
            }
            if (!defer)
                preloadCells(duration);
            if ((defer || forcedProgress) && Debug::V3Diagnostics::streamingWriter().enabled())
            {
                std::ostringstream row;
                row << frame << ',' << Debug::V3Diagnostics::epochMs()
                    << ',' << Debug::V3Diagnostics::csvQuote(defer ? "defer" : "force") << ','
                    << Debug::V3Diagnostics::csvQuote("cell_preload") << ','
                    << Debug::V3Diagnostics::csvQuote(eventDetail) << ',' << lastFrameMs << ','
                    << deferLimit << ',' << deferCount;
                Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
            }
        }
    }

    void Scene::unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard,
        bool hibernateRenderState)
    {
        if (mActiveCells.find(cell) == mActiveCells.end())
            return;
        Debug::V3Diagnostics::writeEvent("unload_cell", cell->getCell()->getDescription());
        Debug::V3Diagnostics::ScopedCsvTimer v3UnloadTimer(
            Debug::V3Diagnostics::transitionWriter(), "unload_cell", cell->getCell()->getDescription());
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();

        // Render state must be captured before ListAndResetObjectsVisitor
        // clears RefData::baseNode for every object in the cell.
        std::size_t v32HibernatedObjects = 0;
        if (hibernateRenderState)
            v32HibernatedObjects = mRendering.hibernateExteriorCell(cell);

        ListAndResetObjectsVisitor visitor;

        cell->forEach(visitor, true); // Include objects being teleported by Lua
        for (const auto& ptr : visitor.mObjects)
        {
            if (const auto object = mPhysics->getObject(ptr))
            {
                if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                    mNavigator.removeObject(DetourNavigator::ObjectId(object), navigatorUpdateGuard);
                mPhysics->remove(ptr);
            }
            else if (mPhysics->getActor(ptr))
            {
                mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
                mRendering.removeActorPath(ptr);
                mPhysics->remove(ptr);
            }
            else
                ptr.mRef->mData.mPhysicsPostponed = false;
            MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        }

        const auto cellX = cell->getCell()->getGridX();
        const auto cellY = cell->getCell()->getGridY();

        if (cell->getCell()->isExterior())
        {
            mNavigator.removeHeightfield(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);
            mPhysics->removeHeightField(cellX, cellY);
        }

        if (cell->getCell()->hasWater())
            mNavigator.removeWater(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.removePathgrid(*pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell->getCell());

        MWBase::Environment::get().getMechanicsManager()->drop(cell);

        if (!hibernateRenderState)
            mRendering.removeCell(cell);
        else if (v32HibernatedObjects != 0)
            Log(Debug::Info) << "V3.2 hibernated " << v32HibernatedObjects << " static render objects from "
                             << cell->getCell()->getDescription();
        MWBase::Environment::get().getWindowManager()->removeCell(cell);

        mWorld.getLocalScripts().clearCell(cell);

        MWBase::Environment::get().getSoundManager()->stopSound(cell);
        mActiveCells.erase(cell);
        // Clean up any effects that may have been spawned while unloading all cells
        if (mActiveCells.empty())
            mRendering.notifyWorldSpaceChanged();
    }

    void Scene::loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn, const osg::Vec3f& position,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3LoadTimer(
            Debug::V3Diagnostics::transitionWriter(), "load_cell", cell.getCell()->getDescription());
        using DetourNavigator::HeightfieldShape;

        assert(mActiveCells.find(&cell) == mActiveCells.end());
        mActiveCells.insert(&cell);

        Log(Debug::Info) << "Loading cell " << cell.getCell()->getDescription();

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();
        const MWWorld::Cell& cellVariant = *cell.getCell();
        ESM::RefId worldspace = cellVariant.getWorldSpace();
        ESM::ExteriorCellLocation cellIndex(cellX, cellY, worldspace);

        if (cellVariant.isExterior())
        {
            osg::ref_ptr<const ESMTerrain::LandObject> land = mRendering.getLandManager()->getLand(cellIndex);
            const ESM::LandData* data = land ? land->getData(ESM::Land::DATA_VHGT) : nullptr;
            const int verts = ESM::getLandSize(worldspace);
            const int worldsize = ESM::getCellSize(worldspace);

            if (data)
            {
                mPhysics->addHeightField(data->getHeights().data(), cellX, cellY, worldsize, verts,
                    data->getMinHeight(), data->getMaxHeight(), land.get());
            }
            else if (!ESM::isEsm4Ext(worldspace))
            {
                static const std::vector<float> defaultHeight(verts * verts, ESM::Land::DEFAULT_HEIGHT);
                mPhysics->addHeightField(defaultHeight.data(), cellX, cellY, worldsize, verts,
                    ESM::Land::DEFAULT_HEIGHT, ESM::Land::DEFAULT_HEIGHT, land.get());
            }
            if (mPhysics->getHeightField(cellX, cellY))
            {
                const osg::Vec2i cellPosition(cellX, cellY);
                const HeightfieldShape shape = [&]() -> HeightfieldShape {
                    if (data == nullptr)
                    {
                        return DetourNavigator::HeightfieldPlane{ static_cast<float>(ESM::Land::DEFAULT_HEIGHT) };
                    }
                    else
                    {
                        DetourNavigator::HeightfieldSurface heights;
                        heights.mHeights = data->getHeights().data();
                        heights.mSize = static_cast<std::size_t>(data->getLandSize());
                        heights.mMinHeight = data->getMinHeight();
                        heights.mMaxHeight = data->getMaxHeight();
                        return heights;
                    }
                }();
                mNavigator.addHeightfield(cellPosition, worldsize, shape, navigatorUpdateGuard);
            }
        }

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.addPathgrid(c, *pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell.getCell());

        // register local scripts
        // do this before insertCell, to make sure we don't add scripts from levelled creature spawning twice
        mWorld.getLocalScripts().addCell(&cell);

        if (respawn)
            cell.respawn();

        std::size_t v32RestoredObjects = 0;
        if (cellVariant.isExterior())
            v32RestoredObjects = mRendering.restoreHibernatedExteriorCell(&cell);
        if (v32RestoredObjects != 0)
            Log(Debug::Info) << "V3.2 restored " << v32RestoredObjects << " static render objects into "
                             << cell.getCell()->getDescription();

        insertCell(cell, loadingListener, navigatorUpdateGuard);

        mRendering.addCell(&cell);

        MWBase::Environment::get().getWindowManager()->addCell(&cell);
        bool waterEnabled = cellVariant.hasWater() || cell.isExterior();
        float waterLevel = cell.getWaterLevel();
        mRendering.setWaterEnabled(waterEnabled);
        if (waterEnabled)
        {
            mPhysics->enableWater(waterLevel);
            mRendering.setWaterHeight(waterLevel);

            if (cellVariant.isExterior())
            {
                if (mPhysics->getHeightField(cellX, cellY))
                    mNavigator.addWater(
                        osg::Vec2i(cellX, cellY), ESM::Land::REAL_SIZE, waterLevel, navigatorUpdateGuard);
            }
            else
            {
                mNavigator.addWater(
                    osg::Vec2i(cellX, cellY), std::numeric_limits<int>::max(), waterLevel, navigatorUpdateGuard);
            }
        }
        else
            mPhysics->disableWater();

        if (!cell.isExterior() && !cellVariant.isQuasiExterior())
            mRendering.configureAmbient(cellVariant);

        mPreloader->notifyLoaded(&cell);
    }

    void Scene::clear()
    {
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            unloadCell(cell, navigatorUpdateGuard.get());
        }
        navigatorUpdateGuard.reset();
        assert(mActiveCells.empty());
        mCurrentCell = nullptr;
        mLowestPoint = std::numeric_limits<float>::max();

        mPreloader->clear();
    }

    osg::Vec4i Scene::gridCenterToBounds(const osg::Vec2i& centerCell) const
    {
        return osg::Vec4i(centerCell.x() - mHalfGridSize, centerCell.y() - mHalfGridSize,
            centerCell.x() + mHalfGridSize + 1, centerCell.y() + mHalfGridSize + 1);
    }

    osg::Vec2i Scene::getNewGridCenter(const osg::Vec3f& pos, const osg::Vec2i* currentGridCenter) const
    {
        ESM::RefId worldspace
            = mCurrentCell ? mCurrentCell->getCell()->getWorldSpace() : ESM::Cell::sDefaultWorldspaceId;
        if (currentGridCenter)
        {
            const osg::Vec2f center = ESM::indexToPosition(
                ESM::ExteriorCellLocation(currentGridCenter->x(), currentGridCenter->y(), worldspace), true);
            float distance = std::max(std::abs(center.x() - pos.x()), std::abs(center.y() - pos.y()));
            int cellSize = ESM::getCellSize(worldspace);
            const float maxDistance = cellSize / 2 + mCellLoadingThreshold; // 1/2 cell size + threshold
            if (distance <= maxDistance)
                return *currentGridCenter;
        }
        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
        return { cellPos.mX, cellPos.mY };
    }

    void Scene::playerMoved(const osg::Vec3f& pos)
    {
        if (!mCurrentCell)
            return;

        // The player is reset when z is 90 units below the lowest reference bound z.
        constexpr float lowestPointAdjustment = -90.0f;
        if (mCurrentCell->isExterior())
        {
            osg::Vec2i newCell = getNewGridCenter(pos, &mCurrentGridCenter);
            if (newCell != mCurrentGridCenter)
                requestChangeCellGrid(pos, newCell);
        }
        else if (pos.z() < mLowestPoint + lowestPointAdjustment)
        {
            // Player has fallen into the void, reset to interior marker/coc (#1415)
            const std::string_view cellNameId = mCurrentCell->getCell()->getNameId();
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWWorld::Ptr playerPtr = world->getPlayerPtr();

            // Check that collision is enabled, which is opposite to Vanilla
            // this change was decided in MR #4100 as the behaviour is preferable
            if (world->isActorCollisionEnabled(playerPtr))
            {
                ESM::Position newPos;
                const ESM::RefId refId = world->findInteriorPosition(cellNameId, newPos);

                // Only teleport if that teleport point is > the lowest point, rare edge case
                if (!refId.empty() && newPos.pos[2] >= mLowestPoint - lowestPointAdjustment)
                {
                    MWWorld::ActionTeleport(refId, newPos, false).execute(playerPtr);
                    Log(Debug::Warning) << "Player position has been reset due to falling into the void";
                }
            }
        }
    }

    void Scene::requestChangeCellGrid(const osg::Vec3f& position, const osg::Vec2i& cell, bool changeEvent)
    {
        mChangeCellGridRequest = ChangeCellGridRequest{ position,
            ESM::ExteriorCellLocation(cell.x(), cell.y(), mCurrentCell->getCell()->getWorldSpace()), changeEvent };
    }

    void Scene::changeCellGrid(const osg::Vec3f& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_cell_grid", "exterior_grid");
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_cell_grid", "exterior_grid", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");
        const int halfGridSize
            = isEsm4Ext(playerCellIndex.mWorldspace) ? Constants::ESM4CellGridRadius : Constants::CellGridRadius;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        const int playerCellX = playerCellIndex.mX;
        const int playerCellY = playerCellIndex.mY;

        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            if (cell->getCell()->isExterior() && cell->getCell()->getWorldSpace() == playerCellIndex.mWorldspace)
            {
                const auto dx = std::abs(playerCellX - cell->getCell()->getGridX());
                const auto dy = std::abs(playerCellY - cell->getCell()->getGridY());
                if (dx > halfGridSize || dy > halfGridSize)
                    unloadCell(cell, navigatorUpdateGuard.get());
            }
            else
                unloadCell(cell, navigatorUpdateGuard.get());
        }

        const DetourNavigator::CellGridBounds cellGridBounds{
            .mCenter = osg::Vec2i(playerCellX, playerCellY),
            .mHalfSize = halfGridSize,
        };

        mNavigator.updateBounds(playerCellIndex.mWorldspace, cellGridBounds, pos, navigatorUpdateGuard.get());

        mHalfGridSize = halfGridSize;
        mCurrentGridCenter = osg::Vec2i(playerCellX, playerCellY);
        osg::Vec4i newGrid = gridCenterToBounds(mCurrentGridCenter);

        // NOTE: setActiveGrid must be after enableTerrain, otherwise we set the grid in the old exterior worldspace
        mRendering.enableTerrain(true, playerCellIndex.mWorldspace);
        mRendering.setActiveGrid(newGrid);

        mPreloader->setTerrain(mRendering.getTerrain());
        if (mRendering.pagingUnlockCache())
            mPreloader->abortTerrainPreloadExcept(nullptr);
        const bool v39NeedsInitialFrontload
            = static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !mV39InitialFrontloadDone;
        if (v39NeedsInitialFrontload
            || !mPreloader->isTerrainLoaded(PositionCellGrid{ pos, newGrid }, mRendering.getReferenceTime()))
            preloadTerrain(pos, playerCellIndex.mWorldspace, true);
        mPagedRefs.clear();
        mRendering.getPagedRefnums(newGrid, mPagedRefs);

        addPostponedPhysicsObjects();

        std::size_t refsToLoad = 0;
        std::vector<std::pair<int, int>> cellsPositionsToLoad;
        iterateOverCellsAround(playerCellX, playerCellY, mHalfGridSize, [&](int x, int y) {
            const ESM::ExteriorCellLocation location(x, y, playerCellIndex.mWorldspace);
            if (isCellInCollection(location, mActiveCells))
                return;
            refsToLoad += mWorld.getWorldModel().getExterior(location).count();
            cellsPositionsToLoad.emplace_back(x, y);
        });

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setLabel("#{OMWEngine:LoadingExterior}");
        loadingListener->setProgressRange(refsToLoad);

        sortCellsToLoad(playerCellX, playerCellY, cellsPositionsToLoad);

        for (const auto& [x, y] : cellsPositionsToLoad)
        {
            ESM::ExteriorCellLocation indexToLoad = { x, y, playerCellIndex.mWorldspace };
            if (!isCellInCollection(indexToLoad, mActiveCells))
            {
                CellStore& cell = mWorld.getWorldModel().getExterior(indexToLoad);
                loadCell(cell, loadingListener, changeEvent, pos, navigatorUpdateGuard.get());
            }
        }

        mRendering.finishExteriorRestore();
        mNavigator.update(pos, navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        CellStore& current = mWorld.getWorldModel().getExterior(playerCellIndex);
        MWBase::Environment::get().getWindowManager()->changeCell(&current);

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;
    }

    void Scene::addPostponedPhysicsObjects()
    {
        for (const auto& cell : mActiveCells)
        {
            cell->forEach([&](const MWWorld::Ptr& ptr) {
                if (ptr.mRef->mData.mPhysicsPostponed)
                {
                    ptr.mRef->mData.mPhysicsPostponed = false;
                    if (ptr.mRef->mData.isEnabled() && ptr.mRef->mRef.getCount() > 0)
                    {
                        const VFS::Path::Normalized model = getModel(ptr);
                        if (!model.empty())
                        {
                            const auto rotation = makeNodeRotation(ptr, RotationOrder::direct);
                            ptr.getClass().insertObjectPhysics(ptr, model, rotation, *mPhysics);
                        }
                    }
                }
                return true;
            });
        }
    }

    void Scene::testExteriorCells()
    {
        // Note: temporary disable ICO to decrease memory usage
        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(nullptr);

        mRendering.getResourceSystem()->setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getExtSize());

        MWWorld::Store<ESM::Cell>::iterator it = cells.extBegin();
        int i = 1;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.extEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingExteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getExtSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getExterior(
                ESM::ExteriorCellLocation(it->mData.mX, it->mData.mY, ESM::Cell::sDefaultWorldspaceId));
            const osg::Vec3f position
                = osg::Vec3f(it->mData.mX + 0.5f, it->mData.mY + 0.5f, 0) * Constants::CellSizeInUnits;
            const osg::Vec2i cellPosition(it->mData.mX, it->mData.mY);

            const DetourNavigator::CellGridBounds cellGridBounds{
                .mCenter = osg::Vec2i(it->mData.mX, it->mData.mY),
                .mHalfSize = Constants::CellGridRadius,
            };

            mNavigator.updateBounds(
                ESM::Cell::sDefaultWorldspaceId, cellGridBounds, position, navigatorUpdateGuard.get());

            loadCell(cell, nullptr, false, position, navigatorUpdateGuard.get());

            mNavigator.update(position, navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                if (it->isExterior() && it->mData.mX == (*iter)->getCell()->getGridX()
                    && it->mData.mY == (*iter)->getCell()->getGridY())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            mRendering.getResourceSystem()->updateCache(mRendering.getReferenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(
            mRendering.getIncrementalCompileOperation());
        mRendering.getResourceSystem()->setExpiryDelay(Settings::RamCache::cacheExpiryDelay());
    }

    void Scene::testInteriorCells()
    {
        // Note: temporary disable ICO to decrease memory usage
        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(nullptr);

        mRendering.getResourceSystem()->setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getIntSize());

        int i = 1;
        MWWorld::Store<ESM::Cell>::iterator it = cells.intBegin();
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.intEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingInteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getIntSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getInterior(it->mName);
            ESM::Position position;
            mWorld.findInteriorPosition(it->mName, position);
            mNavigator.updateBounds(
                cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());
            loadCell(cell, nullptr, false, position.asVec3(), navigatorUpdateGuard.get());

            mNavigator.update(position.asVec3(), navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                assert(!(*iter)->getCell()->isExterior());

                if (it->mName == (*iter)->getCell()->getNameId())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            mRendering.getResourceSystem()->updateCache(mRendering.getReferenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        mRendering.getResourceSystem()->getSceneManager()->setIncrementalCompileOperation(
            mRendering.getIncrementalCompileOperation());
        mRendering.getResourceSystem()->setExpiryDelay(Settings::RamCache::cacheExpiryDelay());
    }

    void Scene::changePlayerCell(CellStore& cell, const ESM::Position& pos, bool adjustPlayerPos)
    {
        mHalfGridSize = cell.getCell()->isEsm4() ? Constants::ESM4CellGridRadius : Constants::CellGridRadius;
        mCurrentCell = &cell;

        mRendering.enableTerrain(cell.isExterior(), cell.getCell()->getWorldSpace());

        MWWorld::Ptr old = mWorld.getPlayerPtr();
        mWorld.getPlayer().setCell(&cell);

        MWWorld::Ptr player = mWorld.getPlayerPtr();
        mRendering.updatePlayerPtr(player);

        // The player is loaded before the scene and by default it is grounded, with the scene fully loaded,
        // we validate and correct this. Only run once, during initial cell load.
        if (old.mCell == &cell)
            mPhysics->traceDown(player, player.getRefData().getPosition().asVec3(), 10.f);

        if (adjustPlayerPos)
        {
            mWorld.moveObject(player, pos.asVec3());
            mWorld.rotateObject(player, pos.asRotationVec3());

            player.getClass().adjustPosition(player, true);
        }

        MWBase::Environment::get().getMechanicsManager()->updateCell(old, player);
        MWBase::Environment::get().getWindowManager()->watchActor(player);

        mPhysics->updatePtr(old, player);

        mWorld.adjustSky();

        mLastPlayerPos = player.getRefData().getPosition().asVec3();
    }

    Scene::Scene(MWWorld::World& world, MWRender::RenderingManager& rendering, MWPhysics::PhysicsSystem* physics,
        DetourNavigator::Navigator& navigator)
        : mCurrentCell(nullptr)
        , mCellChanged(false)
        , mWorld(world)
        , mPhysics(physics)
        , mRendering(rendering)
        , mNavigator(navigator)
        , mCellLoadingThreshold(1024.f)
        , mPreloadDistance(Settings::cells().mPreloadDistance)
        , mPreloadEnabled(Settings::cells().mPreloadEnabled)
        , mPreloadExteriorGrid(Settings::cells().mPreloadExteriorGrid)
        , mPreloadDoors(Settings::cells().mPreloadDoors)
        , mPreloadFastTravel(Settings::cells().mPreloadFastTravel)
        , mV33SpeculativePreloadBudget(Settings::cells().mV33SpeculativePreloadBudget)
        , mPredictionTime(Settings::RamCache::predictionTime())
        , mLowestPoint(std::numeric_limits<float>::max())
    {
        mPreloader = std::make_unique<CellPreloader>(rendering.getResourceSystem(), physics->getShapeManager(),
            rendering.getTerrain(), rendering.getLandManager());
        mPreloader->setWorkQueue(mRendering.getWorkQueue());
        mPreloader->setExpiryDelay(Settings::RamCache::preloadCellExpiryDelay());
        mPreloader->setMinCacheSize(Settings::RamCache::preloadCellCacheMin());
        mPreloader->setMaxCacheSize(Settings::RamCache::preloadCellCacheMax());
        mPreloader->setPreloadInstances(Settings::RamCache::preloadInstances());
    }

    Scene::~Scene()
    {
        for (const osg::ref_ptr<SceneUtil::WorkItem>& v : mWorkItems)
            v->abort();

        for (const osg::ref_ptr<SceneUtil::WorkItem>& v : mWorkItems)
            v->waitTillDone();
    }

    bool Scene::hasCellChanged() const
    {
        return mCellChanged;
    }

    const Scene::CellStoreCollection& Scene::getActiveCells() const
    {
        return mActiveCells;
    }

    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_to_interior", cellName, 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_interior", cellName);
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);
        bool useFading = (mCurrentCell != nullptr);
        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        loadingListener->setLabel("#{OMWEngine:LoadingInterior}");
        Loading::ScopedLoad load(loadingListener);

        if (mCurrentCell == &cell)
        {
            mWorld.moveObject(mWorld.getPlayerPtr(), position.asVec3());
            mWorld.rotateObject(mWorld.getPlayerPtr(), position.asRotationVec3());

            if (adjustPlayerPos)
                mWorld.getPlayerPtr().getClass().adjustPosition(mWorld.getPlayerPtr(), true);
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);
            return;
        }

        Log(Debug::Info) << "Changing to interior";

        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();

        const bool hibernateExterior = mCurrentCell != nullptr && mCurrentCell->isExterior()
            && mRendering.beginExteriorHibernation();

        // unload
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cellToUnload = *iter++;
            unloadCell(cellToUnload, navigatorUpdateGuard.get(), hibernateExterior && cellToUnload->isExterior());
        }
        assert(mActiveCells.empty());

        loadingListener->setProgressRange(cell.count());

        mNavigator.updateBounds(
            cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());

        // Load cell.
        mPagedRefs.clear();
        loadCell(cell, loadingListener, changeEvent, position.asVec3(), navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        changePlayerCell(cell, position, adjustPlayerPos);

        // adjust fog
        mRendering.configureFog(*mCurrentCell->getCell());

        // Sky system
        mWorld.adjustSky();

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;

        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        MWBase::Environment::get().getWindowManager()->changeCell(mCurrentCell);

        MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(cell.getCell()->isQuasiExterior());
    }

    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_exterior");
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_to_exterior", "exterior", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_exterior", "exterior");

        if (changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);
        CellStore& current = mWorld.getWorldModel().getCell(extCellId);

        const osg::Vec2i cellIndex(current.getCell()->getGridX(), current.getCell()->getGridY());

        changeCellGrid(position.asVec3(),
            ESM::ExteriorCellLocation(cellIndex.x(), cellIndex.y(), current.getCell()->getWorldSpace()), changeEvent);

        changePlayerCell(current, position, adjustPlayerPos);

        if (changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(true);
    }

    CellStore* Scene::getCurrentCell()
    {
        return mCurrentCell;
    }

    void Scene::markCellAsUnchanged()
    {
        mCellChanged = false;
    }

    void Scene::insertCell(
        CellStore& cell, Loading::Listener* loadingListener, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        Debug::V3Diagnostics::TraceScope trace("transition", "insert_cell", cell.getCell()->getDescription(), 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3InsertTimer(
            Debug::V3Diagnostics::transitionWriter(), "insert_cell_total", cell.getCell()->getDescription());
        const bool isInterior = !cell.isExterior();
        InsertVisitor insertVisitor(cell, loadingListener);
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::transitionWriter(), "insert_collect_refs", cell.getCell()->getDescription());
            cell.forEach(insertVisitor);
        }

        auto& insertionWriter = Debug::V3Diagnostics::insertionWriter();
        V3InsertionAccumulator insertionStats;
        V3InsertionAccumulatorScope insertionScope(insertionWriter.enabled() ? &insertionStats : nullptr);

        auto& v32RendererWriter = Debug::V3Diagnostics::v32RendererInsertionWriter();
        const bool v32RendererProfileEnabled
            = Settings::cells().mV32RendererInsertionProfiling && v32RendererWriter.enabled();
        Debug::V32RendererProfiling::Stats v32RendererStats;
        Debug::V32RendererProfiling::Scope v32RendererScope(
            v32RendererProfileEnabled ? &v32RendererStats : nullptr);

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::transitionWriter(), "insert_render_physics", cell.getCell()->getDescription());
            insertVisitor.insert(
                [&](const MWWorld::Ptr& ptr) { addObject(ptr, mWorld, mPagedRefs, *mPhysics, mRendering); });
        }
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::transitionWriter(), "insert_nav", cell.getCell()->getDescription());
            insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
                addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator, navigatorUpdateGuard);
            });
        }

        if (insertionWriter.enabled())
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::csvQuote(cell.getCell()->getDescription()) << ',' << insertionStats.mTotalRefs
                << ',' << insertionStats.mRenderedRefs << ',' << insertionStats.mPhysicsRefs << ','
                << insertionStats.mActors << ',' << insertionStats.mAnimated << ',' << insertionStats.mDoors << ','
                << std::fixed << std::setprecision(3) << insertionStats.mRenderMs << ',' << insertionStats.mMechanicsMs
                << ',' << insertionStats.mParticlesMs << ',' << insertionStats.mPhysicsMs << ','
                << insertionStats.mLuaAddedMs << ',' << insertionStats.mNavMs;
            insertionWriter.writeLine(row.str());
        }

        if (v32RendererProfileEnabled)
        {
            const double objectRootExclusiveMs = v32RendererStats.mObjectRootMs > v32RendererStats.mSceneInstanceMs
                ? v32RendererStats.mObjectRootMs - v32RendererStats.mSceneInstanceMs
                : 0.0;
            const double accountedMs = v32RendererStats.mSceneInstanceMs + objectRootExclusiveMs
                + v32RendererStats.mControllerSetupMs + v32RendererStats.mTransformAttachMs;
            const double miscMs = v32RendererStats.mRendererTotalMs > accountedMs
                ? v32RendererStats.mRendererTotalMs - accountedMs
                : 0.0;
            const double meanObjectMs = v32RendererStats.mObjects != 0
                ? v32RendererStats.mRendererTotalMs / static_cast<double>(v32RendererStats.mObjects)
                : 0.0;

            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::csvQuote(cell.getCell()->getDescription()) << ','
                << v32RendererStats.mObjects << ',' << v32RendererStats.mConstructed << ','
                << v32RendererStats.mRestored << ',' << v32RendererStats.mPaged << ','
                << v32RendererStats.mStatic << ',' << v32RendererStats.mAnimated << ','
                << v32RendererStats.mActors << ',' << v32RendererStats.mLights << ',' << std::fixed
                << std::setprecision(3) << v32RendererStats.mRendererTotalMs << ',' << meanObjectMs << ','
                << v32RendererStats.mSceneInstanceMs << ',' << objectRootExclusiveMs << ','
                << v32RendererStats.mControllerSetupMs << ',' << v32RendererStats.mTransformAttachMs << ','
                << miscMs << ',' << v32RendererStats.mMaxObjectMs << ','
                << Debug::V3Diagnostics::csvQuote(v32RendererStats.mMaxRef) << ','
                << Debug::V3Diagnostics::csvQuote(v32RendererStats.mMaxModel);
            v32RendererWriter.writeLine(row.str());
        }
    }

    void Scene::addObjectToScene(const Ptr& ptr)
    {
        const bool isInterior = mCurrentCell && !mCurrentCell->isExterior();
        try
        {
            addObject(ptr, mWorld, mPagedRefs, *mPhysics, mRendering);
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator);
            mWorld.scaleObject(ptr, ptr.getCellRef().getScale());
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
        }
    }

    void Scene::removeObjectFromScene(const Ptr& ptr, bool keepActive)
    {
        MWBase::Environment::get().getMechanicsManager()->remove(ptr, keepActive);
        // You'd expect the sounds attached to the object to be stopped here
        // because the object is nowhere to be heard, but in Morrowind, they're not.
        // They're still stopped when the cell is unloaded
        // or if the player moves away far from the object's position.
        // Todd Howard, Who art in Bethesda, hallowed be Thy name.
        MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        if (const auto object = mPhysics->getObject(ptr))
        {
            if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                mNavigator.removeObject(DetourNavigator::ObjectId(object), nullptr);
        }
        else if (mPhysics->getActor(ptr))
        {
            mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
        }
        mPhysics->remove(ptr);
        mRendering.removeObject(ptr);
        if (ptr.getClass().isActor())
            mRendering.removeWaterRippleEmitter(ptr);
        ptr.getRefData().setBaseNode(nullptr);
    }

    bool Scene::isCellActive(const CellStore& cell)
    {
        return mActiveCells.contains(&cell);
    }

    class PreloadMeshItem : public SceneUtil::WorkItem
    {
    public:
        explicit PreloadMeshItem(VFS::Path::NormalizedView mesh, Resource::SceneManager* sceneManager)
            : mMesh(mesh)
            , mSceneManager(sceneManager)
        {
        }

        void doWork() override
        {
            if (mAborted)
                return;

            try
            {
                mSceneManager->getTemplate(mMesh);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to get mesh template \"" << mMesh << "\" to preload: " << e.what();
            }
        }

        void abort() override { mAborted = true; }

    private:
        VFS::Path::Normalized mMesh;
        Resource::SceneManager* mSceneManager;
        std::atomic_bool mAborted{ false };
    };

    void Scene::preload(const std::string& mesh, bool useAnim)
    {
        const VFS::Path::Normalized meshPath = useAnim
            ? Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mRendering.getResourceSystem()->getVFS())
            : VFS::Path::toNormalized(mesh);

        if (mRendering.getResourceSystem()->getSceneManager()->checkLoaded(meshPath, mRendering.getReferenceTime()))
            return;

        osg::ref_ptr<PreloadMeshItem> item(
            new PreloadMeshItem(meshPath, mRendering.getResourceSystem()->getSceneManager()));
        mRendering.getWorkQueue()->addWorkItem(item);
        const auto isDone = [](const osg::ref_ptr<SceneUtil::WorkItem>& v) { return v->isDone(); };
        mWorkItems.erase(std::remove_if(mWorkItems.begin(), mWorkItems.end(), isDone), mWorkItems.end());
        mWorkItems.emplace_back(std::move(item));
    }

    void Scene::preloadCells(float dt)
    {
        if (dt <= 1e-06)
            return;
        std::vector<PositionCellGrid> exteriorPositions;

        const MWWorld::ConstPtr player = mWorld.getPlayerPtr();
        osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
        osg::Vec3f moved = playerPos - mLastPlayerPos;
        osg::Vec3f predictedPos = playerPos + moved / dt * mPredictionTime;

        if (mCurrentCell->isExterior())
        {
            const int v312PredictorMode = static_cast<int>(Settings::cells().mV312PredictorMode);
            const osg::Vec2i normalPredictedGrid = getNewGridCenter(predictedPos, &mCurrentGridCenter);
            bool etaTargetAdded = false;

            if (v312PredictorMode > 0)
            {
                const osg::Vec3f velocity = moved / dt;
                const ESM::RefId worldspace = mCurrentCell->getCell()->getWorldSpace();
                const osg::Vec2f gridCenter = ESM::indexToPosition(ESM::ExteriorCellLocation(
                    mCurrentGridCenter.x(), mCurrentGridCenter.y(), worldspace), true);
                const float threshold = ESM::getCellSize(worldspace) / 2.f + mCellLoadingThreshold;
                const float leadSeconds = static_cast<float>(Settings::cells().mV312PredictorLeadSeconds);
                constexpr float MinVelocity = 1.f;
                float eta = std::numeric_limits<float>::infinity();

                const auto axisEta = [&](float position, float center, float speed) {
                    if (speed > MinVelocity)
                        return (center + threshold - position) / speed;
                    if (speed < -MinVelocity)
                        return (center - threshold - position) / speed;
                    return std::numeric_limits<float>::infinity();
                };

                const float etaX = axisEta(playerPos.x(), gridCenter.x(), velocity.x());
                const float etaY = axisEta(playerPos.y(), gridCenter.y(), velocity.y());
                if (etaX >= 0.f)
                    eta = std::min(eta, etaX);
                if (etaY >= 0.f)
                    eta = std::min(eta, etaY);

                if (eta <= leadSeconds)
                {
                    osg::Vec3f boundaryProbe = playerPos + velocity * eta;
                    osg::Vec2f horizontalVelocity(velocity.x(), velocity.y());
                    if (horizontalVelocity.length2() > 1.f)
                    {
                        horizontalVelocity.normalize();
                        boundaryProbe.x() += horizontalVelocity.x() * 64.f;
                        boundaryProbe.y() += horizontalVelocity.y() * 64.f;
                    }

                    const osg::Vec2i etaGrid = getNewGridCenter(boundaryProbe, &mCurrentGridCenter);
                    if (etaGrid != mCurrentGridCenter)
                    {
                        exteriorPositions.push_back(
                            PositionCellGrid{ boundaryProbe, gridCenterToBounds(etaGrid) });
                        etaTargetAdded = true;
                        ++mV312EtaTargets;

                        if (v312PredictorMode >= 2 && normalPredictedGrid != etaGrid
                            && normalPredictedGrid != mCurrentGridCenter)
                        {
                            exteriorPositions.push_back(
                                PositionCellGrid{ predictedPos, gridCenterToBounds(normalPredictedGrid) });
                            ++mV312SecondHorizonTargets;
                        }
                    }
                }
            }

            if (!etaTargetAdded)
                exteriorPositions.push_back(
                    PositionCellGrid{ predictedPos, gridCenterToBounds(normalPredictedGrid) });
        }

        mLastPlayerPos = playerPos;

        if (mPreloadEnabled)
        {
            if (mPreloadDoors)
                preloadTeleportDoorDestinations(playerPos, predictedPos);
            if (mPreloadExteriorGrid)
                preloadExteriorGrid(playerPos, predictedPos);
            if (mPreloadFastTravel)
                preloadFastTravelDestinations(playerPos, exteriorPositions);
        }

        mPreloader->setTerrainPreloadPositions(exteriorPositions);
    }

    void Scene::preloadTeleportDoorDestinations(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        std::vector<MWWorld::ConstPtr> teleportDoors;
        for (const MWWorld::CellStore* cellStore : mActiveCells)
        {
            typedef MWWorld::CellRefList<ESM::Door>::List DoorList;
            const DoorList& doors = cellStore->getReadOnlyDoors().mList;
            for (auto& door : doors)
            {
                if (!door.mRef.getTeleport())
                {
                    continue;
                }
                teleportDoors.emplace_back(&door, cellStore);
            }
        }

        for (const MWWorld::ConstPtr& door : teleportDoors)
        {
            float sqrDistToPlayer = (playerPos - door.getRefData().getPosition().asVec3()).length2();
            sqrDistToPlayer
                = std::min(sqrDistToPlayer, (predictedPos - door.getRefData().getPosition().asVec3()).length2());

            if (sqrDistToPlayer < mPreloadDistance * mPreloadDistance)
            {
                try
                {
                    preloadCellWithSurroundings(mWorld.getWorldModel().getCell(door.getCellRef().getDestCell()));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to schedule preload for door " << door.toString() << ": "
                                        << e.what();
                }
            }
        }
    }

    void Scene::preloadExteriorGrid(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos)
    {
        if (!mWorld.isCellExterior())
            return;

        struct Candidate
        {
            CellStore* mCell = nullptr;
            float mPriority = 0.0f;
        };

        const int halfGridSizePlusOne = mHalfGridSize + 1;
        const int cellX = mCurrentGridCenter.x();
        const int cellY = mCurrentGridCenter.y();
        const ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();
        const int cellSize = ESM::getCellSize(extWorldspace);
        const float loadDist = cellSize / 2 + cellSize - mCellLoadingThreshold + mPreloadDistance;
        std::vector<Candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(halfGridSizePlusOne * 8));

        for (int dx = -halfGridSizePlusOne; dx <= halfGridSizePlusOne; ++dx)
        {
            for (int dy = -halfGridSizePlusOne; dy <= halfGridSizePlusOne; ++dy)
            {
                if (dy != halfGridSizePlusOne && dy != -halfGridSizePlusOne && dx != halfGridSizePlusOne
                    && dx != -halfGridSizePlusOne)
                    continue; // only care about the outer (not yet loaded) part of the grid

                const ESM::ExteriorCellLocation cellIndex(cellX + dx, cellY + dy, extWorldspace);
                const osg::Vec2f thisCellCenter = ESM::indexToPosition(cellIndex, true);
                const float playerDist = std::max(
                    std::abs(thisCellCenter.x() - playerPos.x()), std::abs(thisCellCenter.y() - playerPos.y()));
                const float predictedDist = std::max(std::abs(thisCellCenter.x() - predictedPos.x()),
                    std::abs(thisCellCenter.y() - predictedPos.y()));
                if (std::min(playerDist, predictedDist) < loadDist)
                {
                    candidates.push_back(Candidate{ &mWorld.getWorldModel().getExterior(cellIndex),
                        predictedDist + playerDist * 0.25f });
                }
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.mPriority < right.mPriority; });

        unsigned attemptedNew = 0;
        unsigned refreshed = 0;
        unsigned deferred = 0;
        const unsigned configuredBudget = static_cast<unsigned>(mV33SpeculativePreloadBudget);
        unsigned effectiveBudget = configuredBudget;
        bool blockNewPreloads = false;
        const bool v37PressureManagement = static_cast<bool>(Settings::cells().mV32GpuMemoryManagement);
        const Debug::V3GpuMemory::PressureState v37Pressure = v37PressureManagement
            ? Debug::V3GpuMemory::pressureState()
            : Debug::V3GpuMemory::PressureState::Unavailable;
        if (v37Pressure == Debug::V3GpuMemory::PressureState::Hard)
            blockNewPreloads = true;
        else if (v37Pressure == Debug::V3GpuMemory::PressureState::Soft
            && (effectiveBudget == 0 || effectiveBudget > 1))
            effectiveBudget = 1;

        for (const Candidate& candidate : candidates)
        {
            if (mPreloader->isPreloaded(*candidate.mCell))
            {
                preloadCell(*candidate.mCell);
                ++refreshed;
            }
            else if (!blockNewPreloads && (effectiveBudget == 0 || attemptedNew < effectiveBudget))
            {
                preloadCell(*candidate.mCell);
                ++attemptedNew;
            }
            else
                ++deferred;
        }

        if ((configuredBudget != 0 || (v37PressureManagement
                && v37Pressure != Debug::V3GpuMemory::PressureState::Unavailable))
            && Debug::V3Diagnostics::streamingWriter().enabled())
        {
            std::ostringstream detail;
            detail << "attempted_new=" << attemptedNew << ";refreshed=" << refreshed << ";deferred=" << deferred
                   << ";configured_budget=" << configuredBudget << ";effective_budget=" << effectiveBudget
                   << ";block_new=" << (blockNewPreloads ? 1 : 0)
                   << ";pressure=" << Debug::V3GpuMemory::pressureName(v37Pressure);
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ",budget,v37_pressure_aware_preload," << Debug::V3Diagnostics::csvQuote(detail.str()) << ','
                << std::fixed << std::setprecision(3) << Debug::V3HitchTelemetry::lastFrameWallMs() << ','
                << effectiveBudget << ',' << candidates.size();
            Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
        }
    }

    void Scene::preloadCellWithSurroundings(CellStore& cell)
    {
        if (!cell.isExterior())
        {
            mPreloader->preload(cell, mRendering.getReferenceTime());
            return;
        }

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();

        std::vector<std::pair<int, int>> cells;
        const std::size_t gridSize = static_cast<std::size_t>(2 * mHalfGridSize + 1);
        cells.reserve(gridSize * gridSize);

        iterateOverCellsAround(cellX, cellY, mHalfGridSize, [&](int x, int y) { cells.emplace_back(x, y); });

        sortCellsToLoad(cellX, cellY, cells);

        const std::size_t leftCapacity = mPreloader->getMaxCacheSize() - mPreloader->getCacheSize();
        if (cells.size() > leftCapacity)
        {
            [[maybe_unused]] static const bool logged = [&] {
                Log(Debug::Warning) << "Not enough cell preloader cache capacity to preload exterior cells, consider "
                                       "increasing \"preload cell cache max\" up to "
                                    << (mPreloader->getCacheSize() + cells.size());
                return true;
            }();
            cells.resize(leftCapacity);
        }

        const ESM::RefId worldspace = cell.getCell()->getWorldSpace();
        for (const auto& [x, y] : cells)
            mPreloader->preload(mWorld.getWorldModel().getExterior(ESM::ExteriorCellLocation(x, y, worldspace)),
                mRendering.getReferenceTime());
    }

    void Scene::preloadCell(CellStore& cell)
    {
        mPreloader->preload(cell, mRendering.getReferenceTime());
    }

    void Scene::preloadTerrain(const osg::Vec3f& pos, ESM::RefId worldspace, bool sync)
    {
        if (mRendering.getTerrain()->getWorldspace() != worldspace)
            throw std::runtime_error("preloadTerrain can only work with the current exterior worldspace");

        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);
        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;
        const bool v310DoPostTransformFrontload
            = v39DoFrontload && static_cast<bool>(Settings::cells().mV310PreloadPostTransform);
        const bool v310DoFreshFrontload
            = v39DoFrontload
            && (static_cast<bool>(Settings::cells().mV310FreshInitialObjectPaging)
                || v310DoPostTransformFrontload);

        class V310InitialFrontloadScope
        {
        public:
            explicit V310InitialFrontloadScope(MWRender::RenderingManager& rendering)
                : mRendering(rendering)
            {
            }

            void activate()
            {
                if (mActive)
                    return;
                mRendering.setV310InitialFrontloadActive(true);
                mActive = true;
            }

            ~V310InitialFrontloadScope()
            {
                if (mActive)
                    mRendering.setV310InitialFrontloadActive(false);
            }

            V310InitialFrontloadScope(const V310InitialFrontloadScope&) = delete;
            V310InitialFrontloadScope& operator=(const V310InitialFrontloadScope&) = delete;

        private:
            MWRender::RenderingManager& mRendering;
            bool mActive = false;
        };

        V310InitialFrontloadScope v310InitialFrontloadScope(mRendering);

        std::vector<PositionCellGrid> positions;
        if (v39DoFrontload)
        {
            // Cancel a potentially smaller prediction task so the startup task is
            // guaranteed to contain the full requested future-view set. This call
            // waits for the old TerrainPreloadItem to finish before returning.
            mPreloader->abortTerrainPreloadExcept(nullptr);

            // Fresh-cache control is intentionally independent from the optimizer
            // gate so Modes59 and 60 rebuild the same chunk set. Post-transform
            // implicitly requests freshness as a safety net for manual settings.
            if (v310DoFreshFrontload)
                mRendering.clearV310InitialObjectPagingCache();
            if (v310DoPostTransformFrontload)
                v310InitialFrontloadScope.activate();

            const int cellSize = ESM::getCellSize(worldspace);
            // V3.11 prepares the exact neighboring grid-center states normal
            // traversal is most likely to enter next. Older modes retain the
            // historical wider jump for strict backward comparison.
            const bool v311ExactActiveGrid
                = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) > 0;
            const int stepCells = v311ExactActiveGrid ? 1 : std::max(1, mHalfGridSize + 1);

            const auto addView = [&](int dxCells, int dyCells) {
                osg::Vec3f preloadPos = pos;
                preloadPos.x() += static_cast<float>(dxCells * cellSize);
                preloadPos.y() += static_cast<float>(dyCells * cellSize);
                const ESM::ExteriorCellLocation preloadCell = ESM::positionToExteriorCellLocation(
                    preloadPos.x(), preloadPos.y(), worldspace);
                positions.push_back(PositionCellGrid{
                    preloadPos, gridCenterToBounds(osg::Vec2i(preloadCell.mX, preloadCell.mY)) });
            };

            addView(0, 0);
            if (v39FrontloadMode == 1)
            {
                addView(stepCells, 0);
                addView(-stepCells, 0);
                addView(0, stepCells);
                addView(0, -stepCells);
            }
            else
            {
                const int radius = v39FrontloadMode >= 3 ? 2 : 1;
                for (int y = -radius; y <= radius; ++y)
                {
                    for (int x = -radius; x <= radius; ++x)
                    {
                        if (x == 0 && y == 0)
                            continue;
                        addView(x * stepCells, y * stepCells);
                    }
                }
            }

            mPreloader->setTerrainPreloadPositions(positions);
        }
        else
        {
            ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), worldspace);
            const PositionCellGrid position{ pos, gridCenterToBounds({ cellPos.mX, cellPos.mY }) };
            mPreloader->abortTerrainPreloadExcept(&position);
            mPreloader->setTerrainPreloadPositions(std::span(&position, 1));
        }

        if (!sync)
            return;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        loadingListener->setLabel("#{OMWEngine:InitializingData}");
        mPreloader->syncTerrainLoad(*loadingListener);

        if (v39DoFrontload)
            mV39InitialFrontloadDone = true;
    }

    void Scene::reloadTerrain()
    {
        mPreloader->setTerrainPreloadPositions({});
    }

    struct ListFastTravelDestinationsVisitor
    {
        ListFastTravelDestinationsVisitor(float preloadDist, const osg::Vec3f& playerPos)
            : mPreloadDist(preloadDist)
            , mPlayerPos(playerPos)
        {
        }

        bool operator()(const MWWorld::Ptr& ptr)
        {
            if ((ptr.getRefData().getPosition().asVec3() - mPlayerPos).length2() > mPreloadDist * mPreloadDist)
                return true;

            if (ptr.getClass().isNpc())
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::NPC>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            else
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::Creature>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            return true;
        }
        float mPreloadDist;
        osg::Vec3f mPlayerPos;
        std::vector<ESM::Transport::Dest> mList;
    };

    void Scene::preloadFastTravelDestinations(
        const osg::Vec3f& playerPos, std::vector<PositionCellGrid>& exteriorPositions)
    {
        ListFastTravelDestinationsVisitor listVisitor(mPreloadDistance, playerPos);
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();
        for (MWWorld::CellStore* cellStore : mActiveCells)
        {
            cellStore->forEachType<ESM::NPC>(listVisitor);
            cellStore->forEachType<ESM::Creature>(listVisitor);
        }

        for (ESM::Transport::Dest& dest : listVisitor.mList)
        {
            if (!dest.mCellName.empty())
                preloadCell(mWorld.getWorldModel().getInterior(dest.mCellName));
            else
            {
                osg::Vec3f pos = dest.mPos.asVec3();
                const ESM::ExteriorCellLocation cellIndex
                    = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), extWorldspace);
                preloadCellWithSurroundings(mWorld.getWorldModel().getExterior(cellIndex));
                exteriorPositions.push_back(PositionCellGrid{ pos, gridCenterToBounds(getNewGridCenter(pos)) });
            }
        }
    }

    void Scene::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        mPreloader->reportStats(frameNumber, stats);
        stats.setAttribute(frameNumber, "V3.12 ETA Target Selected", static_cast<double>(mV312EtaTargets));
        stats.setAttribute(frameNumber, "V3.12 Second Horizon Target",
            static_cast<double>(mV312SecondHorizonTargets));
    }
}
