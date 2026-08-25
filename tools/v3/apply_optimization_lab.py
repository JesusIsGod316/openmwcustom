from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"optimization-lab patched {rel}")


# ---------------------------------------------------------------------------
# Overdrive RAM mode and an independently selectable preload intensity.
# ---------------------------------------------------------------------------
replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme" }) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme", "overdrive" }) };
        SettingValue<std::string> mRamCacheOverdrivePreload{ mIndex, "Cells", "ram cache overdrive preload",
            makeEnumSanitizerString({ "balanced", "aggressive", "maximum" }) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
)

replace_once(
    "files/settings-default.cfg",
    '''# V3 RAM/cache policy preset.
# normal = upstream OpenMW cache behavior.
# aggressive = longer-lived resource caches and a larger cell preload cache, aimed at systems with spare RAM.
# extreme = 32 GB-class preset. Keeps recently-used cells/resources around for up to 10 minutes and raises
#           preload cache capacity substantially to trade RAM for smoother revisits and cell transitions.
# Presets act as minimums: manually configured values that are already larger are preserved.
ram cache mode = normal

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
    '''# V3 RAM/cache policy preset.
# normal = upstream OpenMW cache behavior.
# aggressive = longer-lived caches for systems with spare RAM.
# extreme = 32 GB-class preset with 10-minute retention and a large preload cache.
# overdrive = experimental 32 GB+ maximum-residency preset. Retains general resources, terrain/object/grass chunks,
#             collision data and raw parsed NIFs much longer so revisits can reuse already-built data.
# Presets act as minimums: manually configured values that are already larger are preserved.
ram cache mode = normal

# Only affects ram cache mode = overdrive.
# balanced keeps speculative preload pressure moderate while retaining data for a long time.
# aggressive and maximum progressively increase preload capacity/prediction for experimentation.
ram cache overdrive preload = balanced

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
)

# Add the overdrive preload policy to the startup log so every profile proves its effective policy.
replace_once(
    "apps/openmw/engine.cpp",
    '''                     << Settings::RamCache::preloadCellCacheMax()
                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s";''',
    '''                     << Settings::RamCache::preloadCellCacheMax()
                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s"
                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName();''',
)

# ---------------------------------------------------------------------------
# Transition lab: nested timings around the exact cell transition path.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);''',
    '''    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_interior", cellName);
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_exterior");

        if (changeEvent)''',
    '''    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_exterior");
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_exterior", "exterior");

        if (changeEvent)''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::changeCellGrid(const osg::Vec3f& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent)
    {
        const int halfGridSize''',
    '''    void Scene::changeCellGrid(const osg::Vec3f& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");
        const int halfGridSize''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn, const osg::Vec3f& position,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        using DetourNavigator::HeightfieldShape;''',
    '''    void Scene::loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn, const osg::Vec3f& position,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3LoadTimer(
            Debug::V3Diagnostics::transitionWriter(), "load_cell", cell.getCell()->getDescription());
        using DetourNavigator::HeightfieldShape;''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Debug::V3Diagnostics::writeEvent("unload_cell", cell->getCell()->getDescription());
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();''',
    '''        Debug::V3Diagnostics::writeEvent("unload_cell", cell->getCell()->getDescription());
        Debug::V3Diagnostics::ScopedCsvTimer v3UnloadTimer(
            Debug::V3Diagnostics::transitionWriter(), "unload_cell", cell->getCell()->getDescription());
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::insertCell(
        CellStore& cell, Loading::Listener* loadingListener, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        const bool isInterior = !cell.isExterior();
        InsertVisitor insertVisitor(cell, loadingListener);
        cell.forEach(insertVisitor);
        insertVisitor.insert(
            [&](const MWWorld::Ptr& ptr) { addObject(ptr, mWorld, mPagedRefs, *mPhysics, mRendering); });
        insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator, navigatorUpdateGuard);
        });
    }''',
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
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        mPreloader->updateCache(mRendering.getReferenceTime());
        preloadCells(duration);''',
    '''        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preloader_cache_update", "", 0.5);
            mPreloader->updateCache(mRendering.getReferenceTime());
        }
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "cell_preload_schedule", "", 0.5);
            preloadCells(duration);
        }''',
)

# ---------------------------------------------------------------------------
# Terrain / object paging / grass residency + creation timing.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
)
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>''',
)
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        const double expiryDelay = Settings::cells().mCacheExpiryDelay;''',
    '''        const double expiryDelay = Settings::RamCache::terrainExpiryDelay();''',
)
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::addCell(const MWWorld::CellStore* store)
    {
        mPathgrid->addCell(store);''',
    '''    void RenderingManager::addCell(const MWWorld::CellStore* store)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3Timer(
            Debug::V3Diagnostics::transitionWriter(), "rendering_add_cell", store->getCell()->getDescription());
        mPathgrid->addCell(store);''',
)
replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);''',
    '''    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        Debug::V3Diagnostics::ScopedCsvTimer v3Timer(
            Debug::V3Diagnostics::transitionWriter(), "rendering_remove_cell", store->getCell()->getDescription());
        mPathgrid->removeCell(store);''',
)

replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''    ObjectPaging::ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace)
        : GenericResourceManager<ChunkId>(nullptr, Settings::cells().mCacheExpiryDelay)''',
    '''    ObjectPaging::ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace)
        : GenericResourceManager<ChunkId>(nullptr, Settings::RamCache::objectPagingExpiryDelay())''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());''',
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());''',
)

replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>''',
)
replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''    Groundcover::Groundcover(
        Resource::SceneManager* sceneManager, float density, float viewDistance, const MWWorld::GroundcoverStore& store)
        : GenericResourceManager<GroundcoverChunkId>(nullptr, Settings::cells().mCacheExpiryDelay)''',
    '''    Groundcover::Groundcover(
        Resource::SceneManager* sceneManager, float density, float viewDistance, const MWWorld::GroundcoverStore& store)
        : GenericResourceManager<GroundcoverChunkId>(nullptr, Settings::RamCache::groundcoverExpiryDelay())''',
)
replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        else
        {
            InstanceMap instances;
            collectInstances(instances, size, center);
            osg::ref_ptr<osg::Node> node = createChunk(instances, center);''',
    '''        else
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
            InstanceMap instances;
            collectInstances(instances, size, center);
            osg::ref_ptr<osg::Node> node = createChunk(instances, center);''',
)

replace_once(
    "components/terrain/chunkmanager.cpp",
    '''#include <components/resource/scenemanager.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/resource/scenemanager.hpp>''',
)
replace_once(
    "components/terrain/chunkmanager.cpp",
    '''        osg::ref_ptr<osg::Node> node = createChunk(size, center, lod, lodFlags, compile, templateGeometry);
        mCache->addEntryToObjectCache(key, node.get());''',
    '''        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::pagingWriter(), "terrain_chunk_create", templateGeometry ? "template_reuse" : "new", 0.25);
        osg::ref_ptr<osg::Node> node = createChunk(size, center, lod, lodFlags, compile, templateGeometry);
        mCache->addEntryToObjectCache(key, node.get());''',
)

# ---------------------------------------------------------------------------
# Resource cache maintenance + Overdrive raw-NIF residency.
# ---------------------------------------------------------------------------
replace_once(
    "components/resource/resourcesystem.cpp",
    '''#include <algorithm>

#include "animblendrulesmanager.hpp"''',
    '''#include <algorithm>

#include <components/debug/v3diagnostics.hpp>
#include <components/settings/ramcache.hpp>

#include "animblendrulesmanager.hpp"''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''        mNifFileManager = std::make_unique<NifFileManager>(vfs, encoder);
        mBgsmFileManager = std::make_unique<BgsmFileManager>(vfs, expiryDelay);''',
    '''        mNifFileManager = std::make_unique<NifFileManager>(vfs, encoder);
        if (Settings::RamCache::retainNifFiles())
            mNifFileManager->setExpiryDelay(expiryDelay);
        mBgsmFileManager = std::make_unique<BgsmFileManager>(vfs, expiryDelay);''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''        // NIF files aren't needed any more once the converted objects are cached in SceneManager / BulletShapeManager,
        // so no point in using an expiry delay
        mNifFileManager->setExpiryDelay(0.0);''',
    '''        // Upstream drops parsed NIFs immediately. Overdrive intentionally retains them so paging and
        // collision paths can reuse already-parsed source data during long sessions.
        mNifFileManager->setExpiryDelay(Settings::RamCache::retainNifFiles() ? expiryDelay : 0.0);''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''    void ResourceSystem::updateCache(double referenceTime)
    {
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->updateCache(referenceTime);
    }''',
    '''    void ResourceSystem::updateCache(double referenceTime)
    {
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::resourceWriter(), "resource_cache_update", "all_managers", 0.5);
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->updateCache(referenceTime);
    }''',
)

# ---------------------------------------------------------------------------
# Navmesh waits/updates: frequent calls are threshold-filtered.
# ---------------------------------------------------------------------------
replace_once(
    "components/detournavigator/navigatorimpl.cpp",
    '''#include <components/esm3/loadpgrd.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/esm3/loadpgrd.hpp>''',
)
replace_once(
    "components/detournavigator/navigatorimpl.cpp",
    '''    void NavigatorImpl::update(const osg::Vec3f& playerPosition, const UpdateGuard* guard)
    {
        removeUnusedNavMeshes();
        mNavMeshManager.update(playerPosition, guard);
    }''',
    '''    void NavigatorImpl::update(const osg::Vec3f& playerPosition, const UpdateGuard* guard)
    {
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::navWriter(), "navigator_update", "", 0.5);
        removeUnusedNavMeshes();
        mNavMeshManager.update(playerPosition, guard);
    }''',
)
replace_once(
    "components/detournavigator/navigatorimpl.cpp",
    '''    void NavigatorImpl::wait(WaitConditionType waitConditionType, Loading::Listener* listener)
    {
        mNavMeshManager.wait(waitConditionType, listener);
    }''',
    '''    void NavigatorImpl::wait(WaitConditionType waitConditionType, Loading::Listener* listener)
    {
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::navWriter(), "navigator_wait", "", 0.1);
        mNavMeshManager.wait(waitConditionType, listener);
    }''',
)

print("V3 Optimization Lab source patch completed successfully.")
