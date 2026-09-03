import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.24 match(es), found {count}: {old[:80]!r}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.24 patched {rel} ({count} match(es))")


def insert_after_line(rel, predicate, lines_to_add):
    path = ROOT / rel
    lines = path.read_text(encoding="utf-8").splitlines()
    indexes = [i for i, line in enumerate(lines) if predicate(line)]
    if len(indexes) != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.24 insertion anchor, found {len(indexes)}")
    i = indexes[0]
    lines[i + 1:i + 1] = lines_to_add
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def insert_at_function_body_start(rel, signature, code):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"{rel}: V3.24 function signature not found: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"{rel}: V3.24 function body not found: {signature}")
    if code.strip() in text[start:text.find("\n    }", brace) if text.find("\n    }", brace) >= 0 else len(text)]:
        raise RuntimeError(f"{rel}: V3.24 code already present in {signature}")
    text = text[:brace + 1] + "\n" + code + text[brace + 1:]
    path.write_text(text, encoding="utf-8", newline="\n")


# V3.24 implementation refinement after source audit:
# - Build the reusable queue-less frame-job QoS primitive now.
# - Mode145 arms/validates the QoS infrastructure with no experimental workload.
# - Mode146 adds a real opportunistic consumer: terrain-only MSOC rasterization on
#   owned immutable inputs with ready-only same-generation publication and ZERO
#   cull-thread wait. The existing full MSOC buffer remains the correctness floor.
# - The originally planned actor/controller consumer is intentionally NOT forced
#   into this build: the safe clone boundary is too narrow to overlap enough work
#   for newly constructed NPCs, while the broader path mutates caches/world/live OSG.
#   It remains gated for a later batched unpublished-preparation implementation.
# - V3.23 stronger 1.5x/2x occlusion tuning remains compiled but OFF in 145/146.

hpp = "components/sceneutil/occlusionculling.hpp"
cpp = "components/sceneutil/occlusionculling.cpp"

replace_exact(
    hpp,
    "#include <iomanip>\n#include <memory>",
    "#include <array>\n#include <atomic>\n#include <cstdint>\n#include <iomanip>\n#include <memory>",
)

replace_exact(
    hpp,
    "        bool renderOccluderTo(MaskedOcclusionCulling* moc, const std::vector<osg::Vec3f>& worldPositions,\n"
    "            const std::vector<unsigned int>& indices) const;",
    "        static bool v324RenderOccluderTo(MaskedOcclusionCulling* moc,\n"
    "            const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices,\n"
    "            const std::array<float, 16>& vp);\n"
    "        bool renderOccluderTo(MaskedOcclusionCulling* moc, const std::vector<osg::Vec3f>& worldPositions,\n"
    "            const std::vector<unsigned int>& indices) const;",
)

replace_exact(
    hpp,
    "        MaskedOcclusionCulling* mV323WorkerMOC = nullptr;\n        bool mFrameActive = false;",
    "        MaskedOcclusionCulling* mV323WorkerMOC = nullptr;\n"
    "        bool mV324FrameJobQos = [] {\n"
    "            const char* value = std::getenv(\"OPENMW_V324_FRAME_JOB_QOS\");\n"
    "            return value && value[0] == '1' && value[1] == '\\0';\n"
    "        }();\n"
    "        bool mV324AsyncMsoc = [] {\n"
    "            const char* value = std::getenv(\"OPENMW_V324_ASYNC_MSOC\");\n"
    "            return value && value[0] == '1' && value[1] == '\\0';\n"
    "        }();\n"
    "        MaskedOcclusionCulling* mV324WorkerMOC = nullptr;\n"
    "        std::uint64_t mV324FrameGeneration = 0;\n"
    "        std::uint64_t mV324TerrainSubmittedGeneration = 0;\n"
    "        std::atomic<std::uint64_t> mV324TerrainReadyGeneration{ 0 };\n"
    "        bool mFrameActive = false;",
)

replace_exact(
    cpp,
    "#include <MaskedOcclusionCulling.h>\n",
    "#include <MaskedOcclusionCulling.h>\n\n#include <components/sceneutil/framejobservice.hpp>\n",
)

# Allocate V3.24's worker-private MOC only when the async mechanism is enabled.
path = ROOT / cpp
text = path.read_text(encoding="utf-8")
ctor = text.find("    OcclusionCuller::OcclusionCuller(")
dtor = text.find("    OcclusionCuller::~OcclusionCuller()", ctor)
if ctor < 0 or dtor < 0:
    raise RuntimeError("V3.24 could not locate OcclusionCuller constructor boundaries")
ctor_block = text[ctor:dtor]
close = ctor_block.rfind("    }\n")
if close < 0:
    raise RuntimeError("V3.24 could not locate OcclusionCuller constructor close")
ctor_insert = '''        if (mV324FrameJobQos && mV324AsyncMsoc && mMOC && mMOCTerrainOnly)\n        {\n            mV324WorkerMOC = MaskedOcclusionCulling::Create();\n            if (mV324WorkerMOC)\n            {\n                mV324WorkerMOC->SetResolution(bufferWidth, bufferHeight);\n                mV324WorkerMOC->SetNearClipPlane(0.1f);\n            }\n        }\n'''
ctor_block = ctor_block[:close] + ctor_insert + ctor_block[close:]
text = text[:ctor] + ctor_block + text[dtor:]
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.24 patched OcclusionCuller constructor")

replace_exact(
    cpp,
    "    OcclusionCuller::~OcclusionCuller()\n    {\n        mV323Worker.reset();",
    "    OcclusionCuller::~OcclusionCuller()\n"
    "    {\n"
    "        if (mV324WorkerMOC)\n"
    "        {\n"
    "            if (mV324TerrainSubmittedGeneration != 0)\n"
    "                FrameJobService::instance().wait(\n"
    "                    FrameJobService::Lane::Opportunistic, mV324TerrainSubmittedGeneration);\n"
    "            MaskedOcclusionCulling::Destroy(mV324WorkerMOC);\n"
    "            mV324WorkerMOC = nullptr;\n"
    "        }\n"
    "        mV323Worker.reset();",
)

replace_exact(
    cpp,
    "        mMOC->ClearBuffer();\n"
    "        if (mMOCTerrainOnly)\n"
    "            mMOCTerrainOnly->ClearBuffer();\n"
    "        if (mV323WorkerMOC)\n"
    "            mV323WorkerMOC->ClearBuffer();",
    "        mMOC->ClearBuffer();\n"
    "        if (mMOCTerrainOnly && !mV324AsyncMsoc)\n"
    "            mMOCTerrainOnly->ClearBuffer();\n"
    "        if (mV323WorkerMOC)\n"
    "            mV323WorkerMOC->ClearBuffer();\n"
    "        if (mV324AsyncMsoc)\n"
    "        {\n"
    "            ++mV324FrameGeneration;\n"
    "            mV324TerrainSubmittedGeneration = 0;\n"
    "        }",
)

# Stateless worker renderer: every input required by the job is owned/copied by
# the submission. It does not read mFrameActive, mViewProjection or mVPFloat.
render_anchor = "    bool OcclusionCuller::renderOccluderTo(MaskedOcclusionCulling* moc,"
text = path.read_text(encoding="utf-8")
pos = text.find(render_anchor)
if pos < 0:
    raise RuntimeError("V3.24 renderOccluderTo anchor not found")
static_renderer = '''    bool OcclusionCuller::v324RenderOccluderTo(MaskedOcclusionCulling* moc,\n        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices,\n        const std::array<float, 16>& vp)\n    {\n        if (!moc || worldPositions.empty() || indices.empty())\n            return false;\n\n        const int numTris = static_cast<int>(indices.size()) / 3;\n        if (numTris <= 0)\n            return false;\n\n        const unsigned int vertexCount = static_cast<unsigned int>(worldPositions.size());\n        for (const auto idx : indices)\n            if (idx >= vertexCount)\n                return false;\n\n        for (const auto& v : worldPositions)\n        {\n            const float w = vp[3] * v.x() + vp[7] * v.y() + vp[11] * v.z() + vp[15];\n            if (!std::isfinite(w) || std::abs(w) < 1e-6f)\n                return false;\n        }\n\n        MaskedOcclusionCulling::VertexLayout vtxLayout(12, 4, 8);\n        moc->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, vp.data(),\n            MaskedOcclusionCulling::BACKFACE_NONE, MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);\n        return true;\n    }\n\n'''
text = text[:pos] + static_renderer + text[pos:]
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.24 inserted stateless owned-input MSOC renderer")

# Add the V3.24 path ahead of V3.23's inherited same-frame worker path. Full-buffer
# terrain raster remains serial/current-frame; only the duplicate terrain-only
# raster becomes opportunistic and ready-only.
async_body = '''        if (mV324FrameJobQos && mV324AsyncMsoc && mV324WorkerMOC)\n        {\n            rasterizeOccluder(worldPositions, indices);\n\n            FrameJobService& jobs = FrameJobService::instance();\n            if (!jobs.isIdle(FrameJobService::Lane::Opportunistic))\n            {\n                jobs.noteSkipped(FrameJobService::Lane::Opportunistic);\n                return;\n            }\n\n            auto positions = std::make_shared<std::vector<osg::Vec3f>>(worldPositions);\n            auto ownedIndices = std::make_shared<std::vector<unsigned int>>(indices);\n            std::array<float, 16> vp{};\n            std::copy(std::begin(mVPFloat), std::end(mVPFloat), vp.begin());\n            const std::uint64_t generation = mV324FrameGeneration;\n            auto* readyGeneration = &mV324TerrainReadyGeneration;\n            MaskedOcclusionCulling* workerMoc = mV324WorkerMOC;\n\n            const bool submitted = jobs.trySubmit(FrameJobService::Lane::Opportunistic, generation,\n                [workerMoc, positions = std::move(positions), ownedIndices = std::move(ownedIndices), vp,\n                    readyGeneration, generation] {\n                    workerMoc->ClearBuffer();\n                    if (OcclusionCuller::v324RenderOccluderTo(workerMoc, *positions, *ownedIndices, vp))\n                        readyGeneration->store(generation, std::memory_order_release);\n                });\n            if (submitted)\n                mV324TerrainSubmittedGeneration = generation;\n            else\n                jobs.noteSkipped(FrameJobService::Lane::Opportunistic);\n            return;\n        }\n'''
insert_at_function_body_start(cpp, "    void OcclusionCuller::rasterizeTerrainOccluder(", async_body)

terrain_test_body = '''        if (mV324FrameJobQos && mV324AsyncMsoc)\n        {\n            if (!mFrameActive || !mV324WorkerMOC)\n                return true;\n\n            const std::uint64_t generation = mV324FrameGeneration;\n            FrameJobService& jobs = FrameJobService::instance();\n            if (mV324TerrainSubmittedGeneration != generation\n                || mV324TerrainReadyGeneration.load(std::memory_order_acquire) != generation\n                || !jobs.isComplete(FrameJobService::Lane::Opportunistic, generation)\n                || jobs.failed(FrameJobService::Lane::Opportunistic, generation))\n                return true; // incomplete/stale/failed parallel depth is fail-open only\n\n            return testVisibleAABBImpl(mV324WorkerMOC, worldBB);\n        }\n'''
insert_at_function_body_start(cpp, "    bool OcclusionCuller::testVisibleAABBTerrainOnly(", terrain_test_body)

replace_exact(
    "apps/openmw/engine.cpp",
    "openmw-custom-v3.23-parallel-msoc",
    "openmw-custom-v3.23-parallel-msoc / openmw-custom-v3.24-frame-job-qos",
)

# Launcher: retain Mode135 as the exact control and V3.23 modes as dormant history.
launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
menu144 = next((line for line in launcher.splitlines() if "144 = V3.23" in line), None)
if not menu144:
    raise RuntimeError("V3.24 launcher lost Mode144 menu anchor")
launcher = launcher.replace(
    menu144 + "\n",
    menu144 + "\n"
    + "Write-Host '145 = V3.24 frame-job QoS infrastructure (no experimental workload)'\n"
    + "Write-Host '146 = V3.24 zero-wait async terrain MSOC on opportunistic QoS lane'\n",
    1,
)

choice_line = next((line for line in launcher.splitlines() if "135-144" in line), None)
if not choice_line:
    raise RuntimeError("V3.24 launcher choice prompt anchor drifted")
new_choice = choice_line.replace("135-144", "135-146", 1)
if ",'144'))" not in new_choice:
    raise RuntimeError("V3.24 launcher choice allowlist anchor drifted")
new_choice = new_choice.replace(",'144'))", ",'144','145','146'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)

v323_var = "$V323ParallelMsocMode = '0'"
if launcher.count(v323_var) != 1:
    raise RuntimeError("V3.24 launcher V3.23 variable anchor drifted")
launcher = launcher.replace(
    v323_var,
    v323_var + "\n$V324FrameJobQos = '0'\n$V324AsyncMsoc = '0'",
    1,
)

line135 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'135'"))
mode135_body = line135[line135.index("{") + 1:line135.rindex("}")].strip()
mode145 = f"        '145' {{ {mode135_body}; $V324FrameJobQos = '1' }}"
mode146 = f"        '146' {{ {mode135_body}; $V324FrameJobQos = '1'; $V324AsyncMsoc = '1' }}"
line144 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'144'"))
launcher = launcher.replace(line144 + "\n", line144 + "\n" + mode145 + "\n" + mode146 + "\n", 1)

manifest_anchor = '    "v323_parallel_msoc_mode=$V323ParallelMsocMode",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.24 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor
    + '\n    "v324_frame_job_qos=$V324FrameJobQos",'
    + '\n    "v324_async_msoc=$V324AsyncMsoc",',
    1,
)

env_anchor = "    $env:OPENMW_V323_PARALLEL_MSOC_MODE = $V323ParallelMsocMode"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.24 launcher environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor
    + "\n    $env:OPENMW_V324_FRAME_JOB_QOS = $V324FrameJobQos"
    + "\n    $env:OPENMW_V324_ASYNC_MSOC = $V324AsyncMsoc",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V323_PARALLEL_MSOC_MODE -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.24 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V324_ASYNC_MSOC -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V324_FRAME_JOB_QOS -ErrorAction SilentlyContinue\n"
    + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")
print("V3.24 patched launcher modes 145-146")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as f:
    f.write('''\n\nV3.24 FRAME-JOB QOS / ZERO-WAIT MSOC\n======================================\nMode135 remains the exact final-V3.21 behavior control. Mode145 enables only the\nV3.24 QoS identity/infrastructure and is expected to be behavior-neutral. Mode146\nuses the opportunistic reserved lane to rasterize the terrain-only MSOC buffer from\nowned current-frame inputs. The main cull thread never waits for this job. If the\nworker is busy, late, stale, or failed, terrain-only queries fail open (visible).\nThe existing full MSOC buffer still rasterizes synchronously and remains the\ncorrectness floor. V3.23 1.5x/2x stronger occlusion tuning is not enabled by 145/146.\n\nActor/controller jobification was deliberately held out after the source audit: the\nprovably safe per-actor clone/remap boundary would require an immediate join for new\nNPC construction and therefore cannot credibly reduce wall time. Broader actor setup\ntouches shared caches, world/mechanics state, animation sources, and live OSG. A\nlater actor build must batch multiple unpublished preparations or identify another\nmeasured >=1 ms (preferably >=2 ms) ownership-clean kernel before worker execution.\n''')

# Rebuild the generated patch/stat after the V3.24 layer so CI and packaged source
# identity describe what will actually compile.
patch_text = subprocess.run(
    ["git", "diff", "--binary"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source.patch").write_text(patch_text, encoding="utf-8", newline="\n")
stat_text = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat_text, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

component_hpp = (ROOT / hpp).read_text(encoding="utf-8")
component_cpp = (ROOT / cpp).read_text(encoding="utf-8")
engine = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")
for marker in (
    "OPENMW_V324_FRAME_JOB_QOS",
    "OPENMW_V324_ASYNC_MSOC",
    "mV324TerrainReadyGeneration",
    "v324RenderOccluderTo",
    "FrameJobService::Lane::Opportunistic",
):
    if marker not in component_hpp + component_cpp:
        raise RuntimeError(f"V3.24 generated MSOC source missing marker: {marker}")
if "openmw-custom-v3.24-frame-job-qos" not in engine:
    raise RuntimeError("V3.24 engine identity marker missing")
for marker in ("145 = V3.24", "146 = V3.24", "v324_frame_job_qos", "v324_async_msoc"):
    if marker not in launcher:
        raise RuntimeError(f"V3.24 launcher missing marker: {marker}")
for number in ("145", "146"):
    line = next(line for line in launcher.splitlines() if line.lstrip().startswith(f"'{number}'"))
    if "$V323ParallelMsocMode = '" in line or "$V322ParallelActorAvoidance = '1'" in line:
        raise RuntimeError(f"V3.24 mode {number} inherited a rejected experimental mode")

print("V3.24 frame-job QoS + zero-wait async terrain MSOC layer passed")
