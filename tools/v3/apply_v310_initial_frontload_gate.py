import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.10 initial-frontload gate match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.10 initial-frontload gate patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.10 QC correction: compile=true is broader than the one-time startup frontload.
# It is also used by later predictive/background terrain preparation. The project
# explicitly prefers paying the expensive post-transform locality pass during the
# first loading screen, NOT letting it become recurring worker contention while
# traversing the world. Add an explicit thread-safe gate around only V3.9's first
# synchronous multi-view frontload.
#
# Two ordering/cache requirements matter:
# - Any older terrain preload must first be aborted and waitTillDone() must return,
#   otherwise an older background task could inherit the startup-only policy.
# - ObjectPaging chunks produced by an earlier prediction use the same ChunkId as
#   the startup request. If retained, those cache hits can bypass the work we are
#   trying to measure. V3.10 therefore exposes a separate fresh-startup rebuild:
#   clear ONLY the ObjectPaging chunk cache after old work is quiescent and before
#   dispatching the new startup preload. Externally referenced nodes stay alive by
#   OSG ref counting; scene templates/images/keyframes are untouched.
#
# This gives a clean causal pair:
#   Mode59: fresh ObjectPaging startup rebuild, no post-transform;
#   Mode60: same fresh rebuild + startup-only post-transform/shared-state;
# while later background preload and synchronous demand misses remain governed by
# V3.9's safer policies.
# -----------------------------------------------------------------------------

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''#include <osg/ref_ptr>\n\n#include <mutex>''',
    '''#include <osg/ref_ptr>\n\n#include <atomic>\n#include <mutex>''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,\n            OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion = false);\n\n    private:''',
    '''        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,\n            OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion = false);\n\n        // V3.10: true only while Scene is synchronously waiting for the one-time\n        // initial multi-view frontload. Worker ObjectPaging construction reads this\n        // flag, so it must be atomic. Later predictive/background preload remains\n        // deliberately outside this gate.\n        void setV310InitialFrontloadActive(bool active)\n        {\n            mV310InitialFrontloadActive.store(active, std::memory_order_release);\n        }\n\n    private:''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        float mMinSizeMergeFactor;\n        float mMinSizeCostMultiplier;\n\n        std::mutex mRefTrackerMutex;''',
    '''        float mMinSizeMergeFactor;\n        float mMinSizeCostMultiplier;\n\n        std::atomic_bool mV310InitialFrontloadActive{ false };\n\n        std::mutex mRefTrackerMutex;''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.hpp",
    '''        bool pagingUnlockCache();\n        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);\n\n        void updateProjectionMatrix();''',
    '''        bool pagingUnlockCache();\n        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);\n        void clearV310InitialObjectPagingCache();\n        void setV310InitialFrontloadActive(bool active);\n\n        void updateProjectionMatrix();''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)\n    {\n        if (mObjectPaging)\n            mObjectPaging->getPagedRefnums(activeGrid, out);\n    }\n\n    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)''',
    '''    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)\n    {\n        if (mObjectPaging)\n            mObjectPaging->getPagedRefnums(activeGrid, out);\n    }\n\n    void RenderingManager::clearV310InitialObjectPagingCache()\n    {\n        if (mObjectPaging)\n        {\n            // A completed prediction may already have populated identical ChunkIds.\n            // clearCache() only drops the cache refs; externally referenced live\n            // nodes remain alive and source scene/image/keyframe caches are untouched.\n            mObjectPaging->clearCache();\n        }\n    }\n\n    void RenderingManager::setV310InitialFrontloadActive(bool active)\n    {\n        if (mObjectPaging)\n            mObjectPaging->setV310InitialFrontloadActive(active);\n    }\n\n    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)''',
)

# The guard starts inactive. V3.9 abortTerrainPreloadExcept(nullptr) first aborts
# the old item and waits for it to finish. Only AFTER that quiescence point may we
# clear ObjectPaging and, for post-transform profiles, enable the atomic gate. The
# RAII destructor guarantees an exception cannot leave the optimizer gate enabled.
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);\n        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;\n\n        std::vector<PositionCellGrid> positions;''',
    '''        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);\n        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;\n        const bool v310DoPostTransformFrontload\n            = v39DoFrontload && static_cast<bool>(Settings::cells().mV310PreloadPostTransform);\n        const bool v310DoFreshFrontload\n            = v39DoFrontload\n            && (static_cast<bool>(Settings::cells().mV310FreshInitialObjectPaging)\n                || v310DoPostTransformFrontload);\n\n        class V310InitialFrontloadScope\n        {\n        public:\n            explicit V310InitialFrontloadScope(MWRender::RenderingManager& rendering)\n                : mRendering(rendering)\n            {\n            }\n\n            void activate()\n            {\n                if (mActive)\n                    return;\n                mRendering.setV310InitialFrontloadActive(true);\n                mActive = true;\n            }\n\n            ~V310InitialFrontloadScope()\n            {\n                if (mActive)\n                    mRendering.setV310InitialFrontloadActive(false);\n            }\n\n            V310InitialFrontloadScope(const V310InitialFrontloadScope&) = delete;\n            V310InitialFrontloadScope& operator=(const V310InitialFrontloadScope&) = delete;\n\n        private:\n            MWRender::RenderingManager& mRendering;\n            bool mActive = false;\n        };\n\n        V310InitialFrontloadScope v310InitialFrontloadScope(mRendering);\n\n        std::vector<PositionCellGrid> positions;''',
)

replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''            // Cancel a potentially smaller prediction task so the startup task is\n            // guaranteed to contain the full requested future-view set.\n            mPreloader->abortTerrainPreloadExcept(nullptr);\n\n            const int cellSize = ESM::getCellSize(worldspace);''',
    '''            // Cancel a potentially smaller prediction task so the startup task is\n            // guaranteed to contain the full requested future-view set. This call\n            // waits for the old TerrainPreloadItem to finish before returning.\n            mPreloader->abortTerrainPreloadExcept(nullptr);\n\n            // Fresh-cache control is intentionally independent from the optimizer\n            // gate so Modes59 and 60 rebuild the same chunk set. Post-transform\n            // implicitly requests freshness as a safety net for manual settings.\n            if (v310DoFreshFrontload)\n                mRendering.clearV310InitialObjectPagingCache();\n            if (v310DoPostTransformFrontload)\n                v310InitialFrontloadScope.activate();\n\n            const int cellSize = ESM::getCellSize(worldspace);''',
)

# Narrow the V3.10 locality override from generic compile=true preload to the
# explicit one-time startup gate. The existing compile check remains as a second
# safety condition and documents the expected worker path.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const bool v310PreloadPostTransform\n                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)\n                && compile && v38BatchingMode >= 2;''',
    '''            const bool v310PreloadPostTransform\n                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)\n                && compile && mV310InitialFrontloadActive.load(std::memory_order_acquire)\n                && v38BatchingMode >= 2;''',
)

print("V3.10 fresh-startup cache control and post-transform gate patched successfully.")
