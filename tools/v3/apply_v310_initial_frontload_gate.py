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
# This keeps three distinct paths:
#   initial frontload: strong mode-2 batching + post-transform + shared state;
#   later background preload: strong V3.9 batching, but no V3.10 post-transform;
#   synchronous demand miss: V3.9 conservative mode-1 + merge-only fallback.
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
    '''        bool pagingUnlockCache();\n        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);\n        void setV310InitialFrontloadActive(bool active);\n\n        void updateProjectionMatrix();''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)\n    {\n        if (mObjectPaging)\n            mObjectPaging->getPagedRefnums(activeGrid, out);\n    }\n\n    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)''',
    '''    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)\n    {\n        if (mObjectPaging)\n            mObjectPaging->getPagedRefnums(activeGrid, out);\n    }\n\n    void RenderingManager::setV310InitialFrontloadActive(bool active)\n    {\n        if (mObjectPaging)\n            mObjectPaging->setV310InitialFrontloadActive(active);\n    }\n\n    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)''',
)

# Activate the gate BEFORE setTerrainPreloadPositions can dispatch worker work.
# The local RAII scope guarantees that an exception from preload cannot leave the
# atomic gate stuck on for later gameplay/background requests.
replace_exact(
    "apps/openmw/mwworld/scene.cpp",
    '''        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);\n        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;\n\n        std::vector<PositionCellGrid> positions;''',
    '''        const int v39FrontloadMode = static_cast<int>(Settings::cells().mV39FrontloadMode);\n        const bool v39DoFrontload = sync && v39FrontloadMode > 0 && !mV39InitialFrontloadDone;\n\n        class V310InitialFrontloadScope\n        {\n        public:\n            V310InitialFrontloadScope(MWRender::RenderingManager& rendering, bool active)\n                : mRendering(rendering)\n                , mActive(active)\n            {\n                if (mActive)\n                    mRendering.setV310InitialFrontloadActive(true);\n            }\n\n            ~V310InitialFrontloadScope()\n            {\n                if (mActive)\n                    mRendering.setV310InitialFrontloadActive(false);\n            }\n\n            V310InitialFrontloadScope(const V310InitialFrontloadScope&) = delete;\n            V310InitialFrontloadScope& operator=(const V310InitialFrontloadScope&) = delete;\n\n        private:\n            MWRender::RenderingManager& mRendering;\n            bool mActive;\n        };\n\n        V310InitialFrontloadScope v310InitialFrontloadScope(mRendering, v39DoFrontload);\n\n        std::vector<PositionCellGrid> positions;''',
)

# Narrow the V3.10 locality override from generic compile=true preload to the
# explicit one-time startup gate. The existing compile check remains as a second
# safety condition and documents the expected worker path.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const bool v310PreloadPostTransform\n                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)\n                && compile && v38BatchingMode >= 2;''',
    '''            const bool v310PreloadPostTransform\n                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)\n                && compile && mV310InitialFrontloadActive.load(std::memory_order_acquire)\n                && v38BatchingMode >= 2;''',
)

print("V3.10 initial-startup-frontload post-transform gate patched successfully.")
