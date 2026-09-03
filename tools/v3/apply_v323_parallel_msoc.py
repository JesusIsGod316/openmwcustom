import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.23 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.23 patched {rel} ({count} match(es))")


def replace_in_block(rel, start_marker, end_marker, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"{rel}: V3.23 start marker not found: {start_marker}")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"{rel}: V3.23 end marker not found: {end_marker}")
    block = text[start:end]
    count = block.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.23 block match(es), found {count}")
    block = block.replace(old, new, expected)
    path.write_text(text[:start] + block + text[end:], encoding="utf-8", newline="\n")
    print(f"V3.23 patched {rel} block {start_marker} ({count} match(es))")


# V3.23 scope:
# - Mode142: exact-policy parallel terrain dual-buffer raster using a dedicated worker.
# - Mode143: Mode142 + two-way batched exact-active PagedOccluderData raster with private MOC merge,
#            1.5x paged distance and 1.5x paged triangle budget.
# - Mode144: Mode143 architecture with 2x paged distance and 2x paged triangle budget.
# - No worker is created, no private MOC is allocated, and no extra callback/batch path is used when off.
# - The shared preload WorkQueue is deliberately not used; same-frame work cannot queue behind preload jobs.
# - V3.22 300-radius CellOcclusion experiment remains off in all V3.23 modes.

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    "#include <iomanip>\n#include <string>\n#include <vector>",
    "#include <iomanip>\n#include <memory>\n#include <string>\n#include <vector>",
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).\n        /// Use for buildings.\n        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);''',
    '''        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).\n        /// Use for buildings.\n        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);\n\n        struct V323OccluderBatchView\n        {\n            const std::vector<osg::Vec3f>* vertices = nullptr;\n            const std::vector<unsigned int>* indices = nullptr;\n        };\n\n        /// V3.23 Mode143/144: split an immutable paged-occluder batch between the\n        /// main MOC and a worker-private MOC, then conservatively merge after the worker completes.\n        void rasterizeOccluderBatch(const std::vector<V323OccluderBatchView>& meshes);''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''    private:\n        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;''',
    '''    private:\n        struct V323ParallelWorker;\n\n        bool renderOccluderTo(MaskedOcclusionCulling* moc, const std::vector<osg::Vec3f>& worldPositions,\n            const std::vector<unsigned int>& indices) const;\n        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        osg::Vec3f mV322EyeWorld;\n        bool mV322ViewInverseValid = false;\n        float mVPFloat[16] = {};\n        bool mFrameActive = false;''',
    '''        osg::Vec3f mV322EyeWorld;\n        bool mV322ViewInverseValid = false;\n        float mVPFloat[16] = {};\n        int mV323ParallelMsocMode = [] {\n            const char* value = std::getenv("OPENMW_V323_PARALLEL_MSOC_MODE");\n            return value && value[0] >= '1' && value[0] <= '3' && value[1] == '\\0' ? value[0] - '0' : 0;\n        }();\n        std::unique_ptr<V323ParallelWorker> mV323Worker;\n        MaskedOcclusionCulling* mV323WorkerMOC = nullptr;\n        bool mFrameActive = false;''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''#include <algorithm>\n#include <cmath>''',
    '''#include <algorithm>\n#include <cmath>\n#include <condition_variable>\n#include <functional>\n#include <mutex>\n#include <thread>''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''namespace SceneUtil\n{\n    OcclusionCuller::OcclusionCuller''',
    '''namespace SceneUtil\n{\n    struct OcclusionCuller::V323ParallelWorker\n    {\n        V323ParallelWorker()\n            : mThread([this] { run(); })\n        {\n        }\n\n        ~V323ParallelWorker()\n        {\n            {\n                std::lock_guard lock(mMutex);\n                mStop = true;\n            }\n            mWork.notify_one();\n            if (mThread.joinable())\n                mThread.join();\n        }\n\n        bool trySubmit(std::function<void()> task)\n        {\n            std::lock_guard lock(mMutex);\n            if (mBusy || mStop)\n                return false;\n            mTask = std::move(task);\n            mBusy = true;\n            mWork.notify_one();\n            return true;\n        }\n\n        void wait()\n        {\n            std::unique_lock lock(mMutex);\n            mDone.wait(lock, [this] { return !mBusy; });\n        }\n\n    private:\n        void run()\n        {\n            while (true)\n            {\n                std::function<void()> task;\n                {\n                    std::unique_lock lock(mMutex);\n                    mWork.wait(lock, [this] { return mStop || mBusy; });\n                    if (mStop)\n                        return;\n                    task = mTask;\n                }\n                task();\n                {\n                    std::lock_guard lock(mMutex);\n                    mTask = {};\n                    mBusy = false;\n                }\n                mDone.notify_all();\n            }\n        }\n\n        std::mutex mMutex;\n        std::condition_variable mWork;\n        std::condition_variable mDone;\n        std::function<void()> mTask;\n        bool mBusy = false;\n        bool mStop = false;\n        std::thread mThread;\n    };\n\n    OcclusionCuller::OcclusionCuller''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''        if (mMOCTerrainOnly)\n        {\n            mMOCTerrainOnly->SetResolution(bufferWidth, bufferHeight);\n            mMOCTerrainOnly->SetNearClipPlane(0.1f);\n        }\n    }''',
    '''        if (mMOCTerrainOnly)\n        {\n            mMOCTerrainOnly->SetResolution(bufferWidth, bufferHeight);\n            mMOCTerrainOnly->SetNearClipPlane(0.1f);\n        }\n\n        if (mV323ParallelMsocMode > 0 && mMOC && mMOCTerrainOnly)\n        {\n            mV323Worker = std::make_unique<V323ParallelWorker>();\n            if (mV323ParallelMsocMode >= 2)\n            {\n                mV323WorkerMOC = MaskedOcclusionCulling::Create();\n                if (mV323WorkerMOC)\n                {\n                    mV323WorkerMOC->SetResolution(bufferWidth, bufferHeight);\n                    mV323WorkerMOC->SetNearClipPlane(0.1f);\n                }\n            }\n        }\n    }''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''    OcclusionCuller::~OcclusionCuller()\n    {\n        if (mMOC)''',
    '''    OcclusionCuller::~OcclusionCuller()\n    {\n        mV323Worker.reset();\n        if (mV323WorkerMOC)\n            MaskedOcclusionCulling::Destroy(mV323WorkerMOC);\n        if (mMOC)''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''        mMOC->ClearBuffer();\n        if (mMOCTerrainOnly)\n            mMOCTerrainOnly->ClearBuffer();''',
    '''        mMOC->ClearBuffer();\n        if (mMOCTerrainOnly)\n            mMOCTerrainOnly->ClearBuffer();\n        if (mV323WorkerMOC)\n            mV323WorkerMOC->ClearBuffer();''',
)

# Replace the terrain/full raster functions together. This preserves the exact legacy path when V3.23 is off.
replace_in_block(
    "components/sceneutil/occlusionculling.cpp",
    "    void OcclusionCuller::rasterizeTerrainOccluder(",
    "    void OcclusionCuller::rasterizeAABBOccluder(",
    '''    void OcclusionCuller::rasterizeTerrainOccluder(\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)\n    {\n        // Rasterize terrain into both buffers so buildings can be tested against\n        // terrain-only depth (via testVisibleAABBTerrainOnly).\n        rasterizeOccluder(worldPositions, indices);\n        if (!mFrameActive || !mMOCTerrainOnly || worldPositions.empty() || indices.empty())\n            return;\n\n        const int numTris = static_cast<int>(indices.size()) / 3;\n        if (numTris <= 0)\n            return;\n\n        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());\n        for (const auto idx : indices)\n            if (idx >= vertexCount)\n                return;\n\n        for (const auto& v : worldPositions)\n        {\n            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];\n            if (!std::isfinite(w) || std::abs(w) < 1e-6f)\n                return;\n        }\n\n        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);\n        mMOCTerrainOnly->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris,\n            mVPFloat, MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);\n    }\n\n    void OcclusionCuller::rasterizeOccluder(\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)\n    {\n        if (!mFrameActive || worldPositions.empty() || indices.empty())\n            return;\n\n        const int numTris = static_cast<int>(indices.size()) / 3;\n        if (numTris <= 0)\n            return;\n\n        // Validate all vertex indices are in range to prevent out-of-bounds reads.\n        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());\n        for (const auto idx : indices)\n        {\n            if (idx >= vertexCount)\n                return;\n        }\n\n        // Pre-transform check: skip triangles with vertices that would produce\n        // extreme clip-space coordinates (w near zero after VP transform).\n        for (const auto& v : worldPositions)\n        {\n            float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];\n            if (!std::isfinite(w) || std::abs(w) < 1e-6f)\n                return;\n        }\n\n        // Vec3f layout: stride=12 bytes, yOffset=4, zOffset=8\n        // MOC treats (x,y,z) as (x,y,w_component) and transforms via the VP matrix\n        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);\n\n        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,\n            MaskedOcclusionCulling::BACKFACE_NONE, // terrain can be seen from below at edges\n            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);\n    }\n\n''',
    '''    bool OcclusionCuller::renderOccluderTo(MaskedOcclusionCulling* moc,\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices) const\n    {\n        if (!mFrameActive || !moc || worldPositions.empty() || indices.empty())\n            return false;\n\n        const int numTris = static_cast<int>(indices.size()) / 3;\n        if (numTris <= 0)\n            return false;\n\n        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());\n        for (const auto idx : indices)\n            if (idx >= vertexCount)\n                return false;\n\n        for (const auto& v : worldPositions)\n        {\n            const float w = mVPFloat[3] * v.x() + mVPFloat[7] * v.y() + mVPFloat[11] * v.z() + mVPFloat[15];\n            if (!std::isfinite(w) || std::abs(w) < 1e-6f)\n                return false;\n        }\n\n        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);\n        moc->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,\n            MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);\n        return true;\n    }\n\n    void OcclusionCuller::rasterizeTerrainOccluder(\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)\n    {\n        // OFF is the original serial path byte-for-byte in behavior: no worker/private buffer participates.\n        if (mV323ParallelMsocMode == 0 || !mV323Worker || !mMOCTerrainOnly)\n        {\n            rasterizeOccluder(worldPositions, indices);\n            renderOccluderTo(mMOCTerrainOnly, worldPositions, indices);\n            return;\n        }\n\n        // The two terrain copies target independent MOC instances. The dedicated worker cannot\n        // be occupied by DatabasePager/preload work, eliminating Mode141's shared-FIFO inversion.\n        const bool submitted = mV323Worker->trySubmit(\n            [this, &worldPositions, &indices] { renderOccluderTo(mMOCTerrainOnly, worldPositions, indices); });\n        if (!submitted)\n        {\n            // Busy-inline fallback: never enqueue and then wait behind unrelated work.\n            rasterizeOccluder(worldPositions, indices);\n            renderOccluderTo(mMOCTerrainOnly, worldPositions, indices);\n            return;\n        }\n\n        rasterizeOccluder(worldPositions, indices);\n        mV323Worker->wait();\n    }\n\n    void OcclusionCuller::rasterizeOccluder(\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)\n    {\n        renderOccluderTo(mMOC, worldPositions, indices);\n    }\n\n    void OcclusionCuller::rasterizeOccluderBatch(const std::vector<V323OccluderBatchView>& meshes)\n    {\n        if (meshes.empty())\n            return;\n\n        if (mV323ParallelMsocMode < 2 || !mV323Worker || !mV323WorkerMOC || meshes.size() < 2)\n        {\n            for (const auto& mesh : meshes)\n                if (mesh.vertices && mesh.indices)\n                    rasterizeOccluder(*mesh.vertices, *mesh.indices);\n            return;\n        }\n\n        mV323WorkerMOC->ClearBuffer();\n        const bool submitted = mV323Worker->trySubmit([this, &meshes] {\n            for (std::size_t i = 1; i < meshes.size(); i += 2)\n            {\n                const auto& mesh = meshes[i];\n                if (mesh.vertices && mesh.indices)\n                    renderOccluderTo(mV323WorkerMOC, *mesh.vertices, *mesh.indices);\n            }\n        });\n        if (!submitted)\n        {\n            for (const auto& mesh : meshes)\n                if (mesh.vertices && mesh.indices)\n                    rasterizeOccluder(*mesh.vertices, *mesh.indices);\n            return;\n        }\n\n        for (std::size_t i = 0; i < meshes.size(); i += 2)\n        {\n            const auto& mesh = meshes[i];\n            if (mesh.vertices && mesh.indices)\n                rasterizeOccluder(*mesh.vertices, *mesh.indices);\n        }\n        mV323Worker->wait();\n        mMOC->MergeBuffer(mV323WorkerMOC);\n    }\n\n''',
)

# App-layer mode plumbing and stronger exact-active paged coverage.
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        bool mV322MsocHotPath = false;\n        osg::Node* mV322PagedDataNode = nullptr;''',
    '''        bool mV322MsocHotPath = false;\n        int mV323ParallelMsocMode = 0;\n        osg::Node* mV322PagedDataNode = nullptr;''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        int v322Cp2OccluderMode()\n        {\n            const char* value = std::getenv("OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE");\n            if (!value || value[0] < '1' || value[0] > '4' || value[1] != '\\0')\n                return 0;\n            return value[0] - '0';\n        }\n\n        std::string_view getModelPathForNode''',
    '''        int v322Cp2OccluderMode()\n        {\n            const char* value = std::getenv("OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE");\n            if (!value || value[0] < '1' || value[0] > '4' || value[1] != '\\0')\n                return 0;\n            return value[0] - '0';\n        }\n\n        int v323ParallelMsocMode()\n        {\n            const char* value = std::getenv("OPENMW_V323_PARALLEL_MSOC_MODE");\n            if (!value || value[0] < '1' || value[0] > '3' || value[1] != '\\0')\n                return 0;\n            return value[0] - '0';\n        }\n\n        std::string_view getModelPathForNode''',
)

replace_in_block(
    "apps/openmw/mwrender/occlusionculling.cpp",
    "    PagedOccluderCallback::PagedOccluderCallback(",
    "    void PagedOccluderCallback::operator()",
    '''        , mMaxTriangles(maxTriangles)\n        , mV322MsocHotPath(v322MsocHotPathEnabled())\n    {\n    }''',
    '''        , mMaxTriangles(maxTriangles)\n        , mV322MsocHotPath(v322MsocHotPathEnabled())\n        , mV323ParallelMsocMode(v323ParallelMsocMode())\n    {\n        if (mV323ParallelMsocMode >= 2)\n        {\n            const float distanceScale = mV323ParallelMsocMode >= 3 ? 2.0f : 1.5f;\n            mMaxDistanceSq *= distanceScale * distanceScale;\n            if (mMaxTriangles > 0)\n            {\n                const unsigned int numerator = mV323ParallelMsocMode >= 3 ? 2u : 3u;\n                const unsigned int denominator = mV323ParallelMsocMode >= 3 ? 1u : 2u;\n                mMaxTriangles = (mMaxTriangles * numerator) / denominator;\n            }\n        }\n    }''',
)

# Limit batching edits to the PagedOccluderCallback body so Cell/groundcover paths are untouched.
path = ROOT / "apps/openmw/mwrender/occlusionculling.cpp"
text = path.read_text(encoding="utf-8")
start = text.find("    void PagedOccluderCallback::operator()")
end = text.find("    CellOcclusionCallback::CellOcclusionCallback", start)
if start < 0 or end < 0:
    raise RuntimeError("apps/openmw/mwrender/occlusionculling.cpp: V3.23 paged callback block not found")
block = text[start:end]
needle = "        PagedOccluderData* pagedData = nullptr;"
if block.count(needle) != 1:
    raise RuntimeError("V3.23 paged callback data anchor drifted")
block = block.replace(
    needle,
    '''        std::vector<SceneUtil::OcclusionCuller::V323OccluderBatchView> v323Batch;\n        PagedOccluderData* pagedData = nullptr;''',
    1,
)
old_raster = '''                mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);\n                mCuller->incrementBuildingOccluders(\n                    newTris, static_cast<unsigned int>(occMesh.vertices.size()));'''
new_raster = '''                if (mV323ParallelMsocMode >= 2)\n                    v323Batch.push_back({ &occMesh.vertices, &occMesh.indices });\n                else\n                    mCuller->rasterizeOccluder(occMesh.vertices, occMesh.indices);\n                mCuller->incrementBuildingOccluders(\n                    newTris, static_cast<unsigned int>(occMesh.vertices.size()));'''
if block.count(old_raster) != 1:
    raise RuntimeError(f"V3.23 paged raster anchor expected once, found {block.count(old_raster)}")
block = block.replace(old_raster, new_raster, 1)
traverse_anchor = "\n        traverse(node, cv);\n"
if block.count(traverse_anchor) != 1:
    raise RuntimeError(f"V3.23 paged traverse anchor expected once, found {block.count(traverse_anchor)}")
block = block.replace(
    traverse_anchor,
    '''\n        if (!v323Batch.empty())\n            mCuller->rasterizeOccluderBatch(v323Batch);\n\n        traverse(node, cv);\n''',
    1,
)
path.write_text(text[:start] + block + text[end:], encoding="utf-8", newline="\n")
print("V3.23 patched PagedOccluderCallback parallel batch")

replace_exact(
    "apps/openmw/engine.cpp",
    '''openmw-custom-v3.22-parallel-architecture-cp1''',
    '''openmw-custom-v3.22-parallel-architecture-cp1 / openmw-custom-v3.23-parallel-msoc''',
)

# Launcher modes 142-144 derive from the uncontaminated Mode135 behavior control.
launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
init_anchor = "$V322ParallelActorAvoidance = '0'\n$V320EngineLuaFastPaths = '0'"
if launcher.count(init_anchor) != 1:
    raise RuntimeError("V3.23 launcher initialization anchor drifted")
launcher = launcher.replace(
    init_anchor,
    "$V322ParallelActorAvoidance = '0'\n$V323ParallelMsocMode = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)
menu141 = "Write-Host '141 = V3.22 parallel immutable actor-avoidance prediction'"
if launcher.count(menu141) != 1:
    raise RuntimeError("V3.23 launcher lost Mode141 menu anchor")
launcher = launcher.replace(
    menu141,
    menu141
    + "\nWrite-Host '142 = V3.23 parallel MSOC parity + dedicated QoS terrain worker'"
    + "\nWrite-Host '143 = V3.23 strong parallel paged MSOC (1.5x range/budget)'"
    + "\nWrite-Host '144 = V3.23 aggressive parallel paged MSOC (2x range/budget, default-off)'",
    1,
)
choice_line = next((line for line in launcher.splitlines() if "Read-Host" in line and "135-141" in line), None)
if not choice_line:
    raise RuntimeError("V3.23 launcher choice prompt anchor drifted")
new_choice = choice_line.replace("135-141", "135-144", 1)
if ",'141'))" not in new_choice:
    raise RuntimeError("V3.23 launcher choice allowlist anchor drifted")
new_choice = new_choice.replace(",'141'))", ",'141','142','143','144'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)
line135 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'135'"))
mode135_body = line135[line135.index("{") + 1 : line135.rindex("}")].strip()
for forbidden in ("$V322CP1MsocHotPath = '1'", "$V322CP2OccluderMode = '1'", "$V322ParallelActorAvoidance = '1'"):
    if forbidden in mode135_body:
        raise RuntimeError(f"V3.23 Mode135 control contaminated by {forbidden}")
if "v322-cp1-v321-control" not in mode135_body:
    raise RuntimeError("V3.23 could not derive modes from Mode135 control")

def make_v323_mode(number, label, mode):
    body = mode135_body.replace("v322-cp1-v321-control", label, 1)
    return f"        '{number}' {{ {body}; $V323ParallelMsocMode = '{mode}' }}"

mode142 = make_v323_mode(142, "v323-parallel-msoc-parity", 1)
mode143 = make_v323_mode(143, "v323-parallel-msoc-strong", 2)
mode144 = make_v323_mode(144, "v323-parallel-msoc-aggressive", 3)
line141 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'141'"))
launcher = launcher.replace(line141 + "\n", line141 + "\n" + mode142 + "\n" + mode143 + "\n" + mode144 + "\n", 1)
manifest_anchor = '    "v322_parallel_actor_avoidance=$V322ParallelActorAvoidance",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.23 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v323_parallel_msoc_mode=$V323ParallelMsocMode",',
    1,
)
env_anchor = "    $env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE = $V322ParallelActorAvoidance"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.23 launcher environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V323_PARALLEL_MSOC_MODE = $V323ParallelMsocMode",
    1,
)
cleanup_anchor = "    Remove-Item Env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.23 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V323_PARALLEL_MSOC_MODE -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.23 — parallel MSOC + frame-critical QoS groundwork
=====================================================

Mode 135 remains the exact final-V3.21 behavior control. V3.22 experimental
mechanisms are compiled but dormant in Modes 142-144 unless explicitly selected.

Mode 142 parallelizes the duplicated terrain raster into the full and terrain-only
MOC buffers. These are independent buffers, so the dedicated V3.23 worker never
shares a live write target with the cull thread. The worker is persistent and is
not the rendering/preload WorkQueue used by Mode141. If it cannot immediately
accept work, the terrain copy runs inline rather than queueing and blocking.

Modes 143 and 144 additionally batch exact-active PagedOccluderData meshes. The
main thread rasterizes alternating meshes into the live MOC while the dedicated
worker rasterizes the other half into a private same-resolution MOC. The worker
is joined only at this bounded private batch and the private buffer is merged
before traversal continues. Mode143 raises only paged-occluder range/budget by
1.5x; Mode144 raises them by 2x. Neither mode lowers CellOcclusion's individual
building radius or re-enables the rejected V3.22 300-radius experiment.

OFF isolation is mandatory: with OPENMW_V323_PARALLEL_MSOC_MODE unset/0, no
V3.23 worker is constructed, no worker MOC is allocated, no paged batch is
formed, and no V3.23 diagnostics are emitted. Mode144 ships compiled and default
off so aggressive coverage can be tested without contaminating the control.
''')

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

# Fail-closed generated-source invariants.
component_hpp = (ROOT / "components/sceneutil/occlusionculling.hpp").read_text(encoding="utf-8")
component_cpp = (ROOT / "components/sceneutil/occlusionculling.cpp").read_text(encoding="utf-8")
app_cpp = (ROOT / "apps/openmw/mwrender/occlusionculling.cpp").read_text(encoding="utf-8")
engine = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")
for marker in (
    "V323ParallelWorker",
    "V323OccluderBatchView",
    "rasterizeOccluderBatch",
    "OPENMW_V323_PARALLEL_MSOC_MODE",
    "MergeBuffer(mV323WorkerMOC)",
):
    if marker not in component_hpp + component_cpp:
        raise RuntimeError(f"V3.23 component source missing marker: {marker}")
for marker in ("v323ParallelMsocMode", "v323Batch", "distanceScale"):
    if marker not in app_cpp:
        raise RuntimeError(f"V3.23 app source missing marker: {marker}")
if "openmw-custom-v3.23-parallel-msoc" not in engine:
    raise RuntimeError("V3.23 engine identity marker missing")
for marker in ("142 = V3.23", "143 = V3.23", "144 = V3.23", "v323_parallel_msoc_mode"):
    if marker not in launcher:
        raise RuntimeError(f"V3.23 launcher missing marker: {marker}")
for number in ("142", "143", "144"):
    line = next(line for line in launcher.splitlines() if line.lstrip().startswith(f"'{number}'"))
    if "$V322ParallelActorAvoidance = '1'" in line or "$V322CP2OccluderMode = '" in line:
        raise RuntimeError(f"V3.23 Mode{number} unexpectedly enables rejected/dormant V3.22 work")

print("V3.23 parallel MSOC generated-source invariants passed")
