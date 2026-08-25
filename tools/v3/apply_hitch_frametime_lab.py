from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"hitch-frametime lab patched {rel}")


# ---------------------------------------------------------------------------
# Runtime-selectable adaptive streaming controls. Default is OFF so the build
# remains behavior-compatible until the experiment is explicitly enabled.
# ---------------------------------------------------------------------------
replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<std::string> mRamCacheOverdrivePreload{ mIndex, "Cells", "ram cache overdrive preload",
            makeEnumSanitizerString({ "balanced", "aggressive", "maximum" }) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        SettingValue<std::string> mRamCacheOverdrivePreload{ mIndex, "Cells", "ram cache overdrive preload",
            makeEnumSanitizerString({ "balanced", "aggressive", "maximum" }) };
        SettingValue<std::string> mV3StreamingScheduler{ mIndex, "Cells", "v3 streaming scheduler",
            makeEnumSanitizerString({ "off", "adaptive" }) };
        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mV3StreamingDistantObjectChunkLimit{ mIndex, "Cells",
            "v3 streaming distant object chunks per frame", makeClampSanitizerInt(1, 64) };
        SettingValue<int> mV3StreamingGroundcoverChunkLimit{ mIndex, "Cells",
            "v3 streaming groundcover chunks per frame", makeClampSanitizerInt(1, 128) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
)

replace_once(
    "files/settings-default.cfg",
    '''ram cache overdrive preload = balanced

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
    '''ram cache overdrive preload = balanced

# V3 experimental walking-stutter scheduler. OFF preserves normal OpenMW behavior.
# adaptive only throttles speculative distant object/groundcover page bursts and predictive preload scheduling
# after an already-slow frame; nearby/required cell and terrain loading is never skipped.
v3 streaming scheduler = off
v3 streaming target frametime = 25
v3 streaming distant object chunks per frame = 2
v3 streaming groundcover chunks per frame = 4

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s"
                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName();''',
    '''                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s"
                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName()
                     << " shape-instance pool=" << Settings::RamCache::shapeInstancePoolSize()
                     << " streaming=" << Settings::cells().mV3StreamingScheduler;''',
)

# ---------------------------------------------------------------------------
# Expose the previous completed frame duration so optional speculative work can
# react to real frame pressure instead of an arbitrary timer.
# ---------------------------------------------------------------------------
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''    inline std::atomic<unsigned> sCurrentFrame{ 0 };

    inline unsigned currentFrame()
    {
        return sCurrentFrame.load(std::memory_order_relaxed);
    }''',
    '''    inline std::atomic<unsigned> sCurrentFrame{ 0 };
    inline std::atomic<double> sLastFrameWallMs{ 0.0 };

    inline unsigned currentFrame()
    {
        return sCurrentFrame.load(std::memory_order_relaxed);
    }

    inline double lastFrameWallMs()
    {
        return sLastFrameWallMs.load(std::memory_order_relaxed);
    }''',
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            if (mStarted)
            {
                const double wallMs = std::chrono::duration<double, std::milli>(now - mFrameStart).count();
                emitPreviousFrame(wallMs);
            }''',
    '''            if (mStarted)
            {
                const double wallMs = std::chrono::duration<double, std::milli>(now - mFrameStart).count();
                sLastFrameWallMs.store(wallMs, std::memory_order_relaxed);
                emitPreviousFrame(wallMs);
            }''',
)

# ---------------------------------------------------------------------------
# Bounded reusable BulletShapeInstance pool. Upstream MultiObjectCache removes
# every unreferenced instance on each cache sweep; Extreme/Overdrive can now
# keep a bounded number instead of recreating those instances while roaming.
# ---------------------------------------------------------------------------
replace_once(
    "components/resource/multiobjectcache.hpp",
    '''#include <map>
#include <mutex>''',
    '''#include <cstddef>
#include <map>
#include <mutex>''',
)
replace_once(
    "components/resource/multiobjectcache.hpp",
    '''        void removeUnreferencedObjectsInCache();''',
    '''        void removeUnreferencedObjectsInCache(std::size_t keepUnreferenced = 0);''',
)
replace_once(
    "components/resource/multiobjectcache.cpp",
    '''    void MultiObjectCache::removeUnreferencedObjectsInCache()
    {
        std::vector<osg::ref_ptr<osg::Object>> objectsToRemove;
        {
            std::lock_guard<std::mutex> lock(mObjectCacheMutex);

            // Remove unreferenced entries from object cache
            ObjectCacheMap::iterator oitr = mObjectCache.begin();
            while (oitr != mObjectCache.end())
            {
                if (oitr->second->referenceCount() <= 1)
                {
                    objectsToRemove.push_back(oitr->second);
                    mObjectCache.erase(oitr++);
                    ++mExpired;
                }
                else
                {
                    ++oitr;
                }
            }
        }

        // note, actual unref happens outside of the lock
        objectsToRemove.clear();
    }''',
    '''    void MultiObjectCache::removeUnreferencedObjectsInCache(std::size_t keepUnreferenced)
    {
        std::vector<osg::ref_ptr<osg::Object>> objectsToRemove;
        {
            std::lock_guard<std::mutex> lock(mObjectCacheMutex);

            // Keep a bounded pool of unused instances for reuse. This preserves
            // upstream behavior when keepUnreferenced == 0.
            std::size_t kept = 0;
            ObjectCacheMap::iterator oitr = mObjectCache.begin();
            while (oitr != mObjectCache.end())
            {
                if (oitr->second->referenceCount() <= 1)
                {
                    if (kept < keepUnreferenced)
                    {
                        ++kept;
                        ++oitr;
                        continue;
                    }
                    objectsToRemove.push_back(oitr->second);
                    mObjectCache.erase(oitr++);
                    ++mExpired;
                }
                else
                {
                    ++oitr;
                }
            }
        }

        // note, actual unref happens outside of the lock
        objectsToRemove.clear();
    }''',
)

replace_once(
    "components/resource/bulletshapemanager.hpp",
    '''#include <osg/ref_ptr>''',
    '''#include <cstddef>

#include <osg/ref_ptr>''',
)
replace_once(
    "components/resource/bulletshapemanager.hpp",
    '''        BulletShapeManager(
            const VFS::Manager* vfs, SceneManager* sceneMgr, NifFileManager* nifFileManager, double expiryDelay);''',
    '''        BulletShapeManager(const VFS::Manager* vfs, SceneManager* sceneMgr, NifFileManager* nifFileManager,
            double expiryDelay, std::size_t instanceCacheKeep = 0);''',
)
replace_once(
    "components/resource/bulletshapemanager.hpp",
    '''        osg::ref_ptr<MultiObjectCache> mInstanceCache;
        SceneManager* mSceneManager;''',
    '''        osg::ref_ptr<MultiObjectCache> mInstanceCache;
        std::size_t mInstanceCacheKeep = 0;
        SceneManager* mSceneManager;''',
)
replace_once(
    "components/resource/bulletshapemanager.cpp",
    '''    BulletShapeManager::BulletShapeManager(
        const VFS::Manager* vfs, SceneManager* sceneMgr, NifFileManager* nifFileManager, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mInstanceCache(new MultiObjectCache)
        , mSceneManager(sceneMgr)''',
    '''    BulletShapeManager::BulletShapeManager(const VFS::Manager* vfs, SceneManager* sceneMgr,
        NifFileManager* nifFileManager, double expiryDelay, std::size_t instanceCacheKeep)
        : ResourceManager(vfs, expiryDelay)
        , mInstanceCache(new MultiObjectCache)
        , mInstanceCacheKeep(instanceCacheKeep)
        , mSceneManager(sceneMgr)''',
)
replace_once(
    "components/resource/bulletshapemanager.cpp",
    '''        mInstanceCache->removeUnreferencedObjectsInCache();''',
    '''        mInstanceCache->removeUnreferencedObjectsInCache(mInstanceCacheKeep);''',
)
replace_once(
    "apps/openmw/mwphysics/physicssystem.cpp",
    '''        , mShapeManager(std::make_unique<Resource::BulletShapeManager>(resourceSystem->getVFS(),
              resourceSystem->getSceneManager(), resourceSystem->getNifFileManager(),
              Settings::RamCache::cacheExpiryDelay()))''',
    '''        , mShapeManager(std::make_unique<Resource::BulletShapeManager>(resourceSystem->getVFS(),
              resourceSystem->getSceneManager(), resourceSystem->getNifFileManager(),
              Settings::RamCache::cacheExpiryDelay(), Settings::RamCache::shapeInstancePoolSize()))''',
)

# ---------------------------------------------------------------------------
# Deep per-cell object insertion aggregation. This adds very little overhead
# when disabled and tells us whether runtime reconstruction time is cloning,
# mechanics, particles, physics, Lua registration, or navigator insertion.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    static osg::ref_ptr<SceneUtil::PositionAttitudeTransform> pagedNode = new SceneUtil::PositionAttitudeTransform;

    void addObject''',
    '''    static osg::ref_ptr<SceneUtil::PositionAttitudeTransform> pagedNode = new SceneUtil::PositionAttitudeTransform;

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

    void addObject''',
)

old_add_object = '''    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const std::vector<ESM::RefNum>& pagedRefs,
        MWPhysics::PhysicsSystem& physics, MWRender::RenderingManager& rendering)
    {
        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
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
        setNodeRotation(ptr, rendering, rotation);

        if (ptr.getClass().useAnim())
            MWBase::Environment::get().getMechanicsManager()->add(ptr);

        if (ptr.getClass().isActor())
            rendering.addWaterRippleEmitter(ptr);

        // Restore effect particles
        world.applyLoopingParticles(ptr);

        if (!model.empty())
            ptr.getClass().insertObject(ptr, model, rotation, physics);

        MWBase::Environment::get().getLuaManager()->objectAddedToScene(ptr);
    }'''
new_add_object = '''    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const std::vector<ESM::RefNum>& pagedRefs,
        MWPhysics::PhysicsSystem& physics, MWRender::RenderingManager& rendering)
    {
        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
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
        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (!paged)
        {
            ptr.getClass().insertObjectRendering(ptr, model, rendering);
            if (stats)
                ++stats->mRenderedRefs;
        }
        else
            ptr.getRefData().setBaseNode(pagedNode);
        setNodeRotation(ptr, rendering, rotation);
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
        // Restore effect particles
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
    }'''
replace_once("apps/openmw/mwworld/scene.cpp", old_add_object, new_add_object)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const MWPhysics::PhysicsSystem& physics,
        float& lowestPoint, bool isInterior, DetourNavigator::Navigator& navigator,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard = nullptr)
    {
        if (const auto object = physics.getObject(ptr))''',
    '''    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const MWPhysics::PhysicsSystem& physics,
        float& lowestPoint, bool isInterior, DetourNavigator::Navigator& navigator,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard = nullptr)
    {
        V3InsertionAccumulator* const stats = sV3InsertionAccumulator;
        const auto navStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (const auto object = physics.getObject(ptr))''',
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''            if (!navigator.addAgent(agentBounds))
                Log(Debug::Warning) << "Agent bounds are not supported by navigator for " << ptr.toString() << ": "
                                    << agentBounds;
        }
    }

    struct InsertVisitor''',
    '''            if (!navigator.addAgent(agentBounds))
                Log(Debug::Warning) << "Agent bounds are not supported by navigator for " << ptr.toString() << ": "
                                    << agentBounds;
        }
        if (stats)
            stats->mNavMs += Debug::V3Diagnostics::elapsedMs(navStart);
    }

    struct InsertVisitor''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::insertCell(
        CellStore& cell, Loading::Listener* loadingListener, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3InsertTimer(
            Debug::V3Diagnostics::transitionWriter(), "insert_cell_total", cell.getCell()->getDescription());
        const bool isInterior = !cell.isExterior();
        InsertVisitor insertVisitor(cell, loadingListener);
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::transitionWriter(), "insert_collect_refs", cell.getCell()->getDescription());
            cell.forEach(insertVisitor);
        }
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
    }''',
    '''    void Scene::insertCell(
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
        V3InsertionAccumulator* const previousStats = sV3InsertionAccumulator;
        if (insertionWriter.enabled())
            sV3InsertionAccumulator = &insertionStats;

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

        sV3InsertionAccumulator = previousStats;
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
    }''',
)

# ---------------------------------------------------------------------------
# Adaptive predictive preload: after a frame already exceeded the target,
# schedule speculative cell preloads every other frame until pressure subsides.
# Required cell loading is elsewhere and remains synchronous/unchanged.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preload_schedule", "", 0.5);
            preloadCells(duration);
        }''',
    '''        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preload_schedule", "", 0.5);
            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            const bool pressure = Settings::RamCache::adaptiveStreamingEnabled()
                && lastFrameMs > Settings::RamCache::streamingTargetFrameMs();
            const bool defer = pressure && (Debug::V3HitchTelemetry::currentFrame() & 1u);
            if (!defer)
                preloadCells(duration);
            else if (Debug::V3Diagnostics::streamingWriter().enabled())
            {
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                    << ",\"defer\",\"cell_preload\",\"pressure\"," << lastFrameMs << ",1,1";
                Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
            }
        }''',
)

# ---------------------------------------------------------------------------
# Optional distant-object and groundcover burst limiter. It only engages after
# an over-budget frame, never applies to active-grid object pages, and is OFF by
# default. Returning nullptr leaves the page uncached so the quadtree can retry.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);''',
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        if (!activeGrid && Settings::RamCache::adaptiveStreamingEnabled())
        {
            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            if (lastFrameMs > Settings::RamCache::streamingTargetFrameMs())
            {
                static thread_local unsigned v3Frame = std::numeric_limits<unsigned>::max();
                static thread_local int v3Created = 0;
                const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
                if (v3Frame != frame)
                {
                    v3Frame = frame;
                    v3Created = 0;
                }
                const int limit = Settings::RamCache::streamingDistantObjectChunkLimit();
                if (v3Created >= limit)
                {
                    if (Debug::V3Diagnostics::streamingWriter().enabled())
                    {
                        std::ostringstream row;
                        row << frame << ',' << Debug::V3Diagnostics::epochMs()
                            << ",\"defer\",\"object_chunk\",\"distant\"," << lastFrameMs << ',' << limit << ','
                            << v3Created;
                        Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
                    }
                    return nullptr;
                }
                ++v3Created;
            }
        }
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);''',
)

replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        else
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
            InstanceMap instances;''',
    '''        else
        {
            if (Settings::RamCache::adaptiveStreamingEnabled())
            {
                const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
                if (lastFrameMs > Settings::RamCache::streamingTargetFrameMs())
                {
                    static thread_local unsigned v3Frame = std::numeric_limits<unsigned>::max();
                    static thread_local int v3Created = 0;
                    const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
                    if (v3Frame != frame)
                    {
                        v3Frame = frame;
                        v3Created = 0;
                    }
                    const int limit = Settings::RamCache::streamingGroundcoverChunkLimit();
                    if (v3Created >= limit)
                    {
                        if (Debug::V3Diagnostics::streamingWriter().enabled())
                        {
                            std::ostringstream row;
                            row << frame << ',' << Debug::V3Diagnostics::epochMs()
                                << ",\"defer\",\"groundcover_chunk\",\"distant\"," << lastFrameMs << ',' << limit
                                << ',' << v3Created;
                            Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
                        }
                        return nullptr;
                    }
                    ++v3Created;
                }
            }
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
            InstanceMap instances;''',
)

# ---------------------------------------------------------------------------
# WorkQueue/thread critical-path telemetry. This is opt-in and gives us task
# type, queue depth, worker activity and task runtime plus nested trace IDs.
# ---------------------------------------------------------------------------
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''#include <components/debug/debuglog.hpp>

#include <numeric>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>

#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <typeinfo>''',
)

old_add_work = '''    void WorkQueue::addWorkItem(osg::ref_ptr<WorkItem> item, bool front)
    {
        if (item->isDone())
        {
            Log(Debug::Error) << "Error: trying to add a work item that is already completed";
            return;
        }

        std::unique_lock<std::mutex> lock(mMutex);
        if (front)
            mQueue.push_front(std::move(item));
        else
            mQueue.push_back(std::move(item));
        mCondition.notify_one();
    }'''
new_add_work = '''    void WorkQueue::addWorkItem(osg::ref_ptr<WorkItem> item, bool front)
    {
        if (item->isDone())
        {
            Log(Debug::Error) << "Error: trying to add a work item that is already completed";
            return;
        }

        auto& writer = Debug::V3Diagnostics::workQueueWriter();
        const bool profile = writer.enabled();
        const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
        const std::string typeName = profile ? typeid(*item).name() : std::string();
        std::size_t queueDepth = 0;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (front)
                mQueue.push_front(std::move(item));
            else
                mQueue.push_back(std::move(item));
            queueDepth = mQueue.size();
            mCondition.notify_one();
        }
        if (profile)
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::threadId() << ",\"enqueue\"," << itemId << ','
                << Debug::V3Diagnostics::csvQuote(typeName) << ',' << queueDepth << ',' << getNumActiveThreads() << ",0";
            writer.writeLine(row.str());
        }
    }'''
replace_once("components/sceneutil/workqueue.cpp", old_add_work, new_add_work)

old_run = '''    void WorkThread::run()
    {
        while (true)
        {
            osg::ref_ptr<WorkItem> item = mWorkQueue->removeWorkItem();
            if (!item)
                return;
            mActive = true;
            item->doWork();
            item->signalDone();
            mActive = false;
        }
    }'''
new_run = '''    void WorkThread::run()
    {
        while (true)
        {
            osg::ref_ptr<WorkItem> item = mWorkQueue->removeWorkItem();
            if (!item)
                return;
            mActive = true;

            auto& writer = Debug::V3Diagnostics::workQueueWriter();
            const bool profile = writer.enabled();
            const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
            const std::string typeName = profile ? typeid(*item).name() : std::string();
            const auto start = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            Debug::V3Diagnostics::TraceScope trace("workqueue", typeName, std::to_string(itemId), 0.05);

            if (profile)
            {
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                    << Debug::V3Diagnostics::threadId() << ",\"start\"," << itemId << ','
                    << Debug::V3Diagnostics::csvQuote(typeName) << ',' << mWorkQueue->getNumItems() << ','
                    << mWorkQueue->getNumActiveThreads() << ",0";
                writer.writeLine(row.str());
            }

            item->doWork();
            item->signalDone();

            if (profile)
            {
                const double durationMs = Debug::V3Diagnostics::elapsedMs(start);
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                    << Debug::V3Diagnostics::threadId() << ",\"end\"," << itemId << ','
                    << Debug::V3Diagnostics::csvQuote(typeName) << ',' << mWorkQueue->getNumItems() << ','
                    << mWorkQueue->getNumActiveThreads() << ',' << std::fixed << std::setprecision(3) << durationMs;
                writer.writeLine(row.str());
            }
            mActive = false;
        }
    }'''
replace_once("components/sceneutil/workqueue.cpp", old_run, new_run)

print("V3 Hitch + Frametime Lab source patch completed successfully.")
