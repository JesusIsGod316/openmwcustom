from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"prepared-instance lab patched {rel}")


# ---------------------------------------------------------------------------
# Runtime setting. OFF preserves normal OpenMW behavior. The pool is global,
# bounded, and only accepts scene templates with no update traversal so the
# preload worker targets static architecture/props rather than actors or
# particle/update-heavy objects.
# ---------------------------------------------------------------------------
replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mV3PreparedInstanceCache{ mIndex, "Cells", "v3 prepared instance cache" };
        SettingValue<int> mV3PreparedInstanceCacheMax{ mIndex, "Cells", "v3 prepared instance cache max",
            makeClampSanitizerInt(256, 65536) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
)

replace_once(
    "files/settings-default.cfg",
    '''v3 streaming scheduler = off
v3 streaming target frametime = 25

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
    '''v3 streaming scheduler = off
v3 streaming target frametime = 25

# V3 experimental prepared static-instance pool. The preload worker clones safe static scene instances ahead of time,
# and cell activation consumes those already-prepared clones instead of cloning the same templates on the main thread.
# OFF preserves normal OpenMW behavior. Start around 8192 on a 32 GB system; the pool is strictly bounded.
v3 prepared instance cache = false
v3 prepared instance cache max = 8192

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
)

# Startup log proves the effective experiment state in every profile.
replace_once(
    "apps/openmw/engine.cpp",
    '''                     << " shape-instance pool=" << Settings::RamCache::shapeInstancePoolSize()
                     << " streaming=" << Settings::cells().mV3StreamingScheduler;''',
    '''                     << " shape-instance pool=" << Settings::RamCache::shapeInstancePoolSize()
                     << " streaming=" << Settings::cells().mV3StreamingScheduler
                     << " prepared instances=" << (Settings::cells().mV3PreparedInstanceCache ? "on" : "off")
                     << "/" << Settings::cells().mV3PreparedInstanceCacheMax;''',
)

# The app layer owns policy. Resource/SceneManager stays independent of the
# settings subsystem; it just receives a numeric pool limit.
replace_once(
    "apps/openmw/engine.cpp",
    '''    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);''',
    '''    mResourceSystem->getSceneManager()->setPreparedInstanceCacheLimit(
        Settings::cells().mV3PreparedInstanceCache
            ? static_cast<std::size_t>(Settings::cells().mV3PreparedInstanceCacheMax)
            : 0u);
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);''',
)

# ---------------------------------------------------------------------------
# Thread-safe prepared-instance pool in SceneManager.
# ---------------------------------------------------------------------------
replace_once(
    "components/resource/scenemanager.hpp",
    '''#include <array>
#include <memory>
#include <mutex>
#include <string>''',
    '''#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>''',
)

replace_once(
    "components/resource/scenemanager.hpp",
    '''        /// Instance the given scene template.
        /// @see getTemplate
        /// @note Thread safe.
        osg::ref_ptr<osg::Node> getInstance(VFS::Path::NormalizedView path);''',
    '''        /// Instance the given scene template.
        /// @see getTemplate
        /// @note Thread safe.
        osg::ref_ptr<osg::Node> getInstance(VFS::Path::NormalizedView path);

        /// Configure a bounded pool of scene instances prepared by preload workers.
        /// A limit of zero disables the experiment and clears all prepared instances.
        void setPreparedInstanceCacheLimit(std::size_t limit);

        /// Prepare one reusable instance for a future getInstance(path) call.
        /// Only templates with no update traversal are accepted. Thread safe.
        bool prepareInstance(VFS::Path::NormalizedView path);''',
)

replace_once(
    "components/resource/scenemanager.hpp",
    '''        mutable std::mutex mSharedStateMutex;

        std::unique_ptr<Shader::ShaderManager> mShaderManager;''',
    '''        mutable std::mutex mSharedStateMutex;

        mutable std::mutex mPreparedInstanceMutex;
        std::map<VFS::Path::Normalized, std::deque<osg::ref_ptr<osg::Node>>, std::less<>> mPreparedInstances;
        std::size_t mPreparedInstanceLimit = 0;
        std::size_t mPreparedInstanceCount = 0;
        std::size_t mPreparedInstanceAdded = 0;
        std::size_t mPreparedInstanceHits = 0;
        std::size_t mPreparedInstanceMisses = 0;
        std::size_t mPreparedInstanceRejected = 0;

        std::unique_ptr<Shader::ShaderManager> mShaderManager;''',
)

# Render-futureproof has already wrapped getInstance(path) with diagnostics by
# the time this patch runs. Consume a prepared clone first, otherwise fall back
# to the exact normal getTemplate -> getInstance path.
replace_once(
    "components/resource/scenemanager.cpp",
    '''    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path)
    {
        Debug::V3Diagnostics::TraceScope trace("render", "scene_instance", path.value(), 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::renderWriter(), "scene_instance", path.value(), 0.25);
        return getInstance(getTemplate(path));
    }''',
    '''    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path)
    {
        Debug::V3Diagnostics::TraceScope trace("render", "scene_instance", path.value(), 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::renderWriter(), "scene_instance", path.value(), 0.25);
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit != 0)
            {
                const auto found = mPreparedInstances.find(path);
                if (found != mPreparedInstances.end() && !found->second.empty())
                {
                    osg::ref_ptr<osg::Node> prepared = std::move(found->second.front());
                    found->second.pop_front();
                    --mPreparedInstanceCount;
                    ++mPreparedInstanceHits;
                    if (found->second.empty())
                        mPreparedInstances.erase(found);
                    return prepared;
                }
                ++mPreparedInstanceMisses;
            }
        }
        return getInstance(getTemplate(path));
    }

    void SceneManager::setPreparedInstanceCacheLimit(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        mPreparedInstanceLimit = limit;
        while (mPreparedInstanceCount > mPreparedInstanceLimit && !mPreparedInstances.empty())
        {
            auto it = mPreparedInstances.begin();
            while (it != mPreparedInstances.end() && mPreparedInstanceCount > mPreparedInstanceLimit)
            {
                while (!it->second.empty() && mPreparedInstanceCount > mPreparedInstanceLimit)
                {
                    it->second.pop_front();
                    --mPreparedInstanceCount;
                }
                if (it->second.empty())
                    it = mPreparedInstances.erase(it);
                else
                    ++it;
            }
        }
    }

    bool SceneManager::prepareInstance(VFS::Path::NormalizedView path)
    {
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit == 0 || mPreparedInstanceCount >= mPreparedInstanceLimit)
                return false;
        }

        osg::ref_ptr<const osg::Node> sceneTemplate = getTemplate(path);
        if (!sceneTemplate || sceneTemplate->getNumChildrenRequiringUpdateTraversal() != 0)
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            ++mPreparedInstanceRejected;
            return false;
        }

        // cloneNode/getInstance(const Node*) is explicitly thread safe in SceneManager.
        // No update-traversal templates are accepted, avoiding actors/particle-heavy nodes.
        osg::ref_ptr<osg::Node> prepared = getInstance(sceneTemplate.get());
        if (!prepared)
            return false;

        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        if (mPreparedInstanceLimit == 0 || mPreparedInstanceCount >= mPreparedInstanceLimit)
            return false;
        mPreparedInstances[VFS::Path::Normalized(path)].push_back(std::move(prepared));
        ++mPreparedInstanceCount;
        ++mPreparedInstanceAdded;
        return true;
    }''',
)

# Clear prepared clones with the rest of the scene cache so configuration/world
# resets cannot leave stale instances alive.
replace_once(
    "components/resource/scenemanager.cpp",
    '''    void SceneManager::clearCache()
    {
        ResourceManager::clearCache();

        std::lock_guard<std::mutex> lock(mSharedStateMutex);
        mSharedStateManager->clearCache();
    }''',
    '''    void SceneManager::clearCache()
    {
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            mPreparedInstances.clear();
            mPreparedInstanceCount = 0;
        }
        ResourceManager::clearCache();

        std::lock_guard<std::mutex> lock(mSharedStateMutex);
        mSharedStateManager->clearCache();
    }''',
)

# OSG resource stats give us cheap per-frame pool occupancy/hit/miss counters in
# the existing stats log without creating another high-volume CSV.
replace_once(
    "components/resource/scenemanager.cpp",
    '''        Resource::reportStats("Node", frameNumber, mCache->getStats(), *stats);
    }''',
    '''        Resource::reportStats("Node", frameNumber, mCache->getStats(), *stats);
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            stats->setAttribute(frameNumber, "Prepared Instance Count", static_cast<double>(mPreparedInstanceCount));
            stats->setAttribute(frameNumber, "Prepared Instance Added", static_cast<double>(mPreparedInstanceAdded));
            stats->setAttribute(frameNumber, "Prepared Instance Hit", static_cast<double>(mPreparedInstanceHits));
            stats->setAttribute(frameNumber, "Prepared Instance Miss", static_cast<double>(mPreparedInstanceMisses));
            stats->setAttribute(frameNumber, "Prepared Instance Rejected", static_cast<double>(mPreparedInstanceRejected));
        }
    }''',
)

# ---------------------------------------------------------------------------
# Preload worker: current mMeshes intentionally contains one entry per object's
# preload request, so repeated static models naturally create multiple prepared
# clones for a city cell. The SceneManager pool is bounded globally.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    if (mPreloadInstances)
                        mPreloadedObjects.insert(mBulletShapeManager->cacheInstance(mesh));''',
    '''                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));
                    mSceneManager->prepareInstance(mesh);
                    if (mPreloadInstances)
                        mPreloadedObjects.insert(mBulletShapeManager->cacheInstance(mesh));''',
)

print("V3 Prepared Static Instance Lab source patch completed successfully.")
