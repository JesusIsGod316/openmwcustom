import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.22 CP1 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.22 CP1 patched {rel} ({count} match(es))")


# CP1 scope lock:
# - preserve final V3.21 visibility decisions and exact-active ObjectPaging topology;
# - use only osg::Matrixd::invert/ref_ptr/UserData APIs already present in V3.21;
# - no new OSG frustum API, MOC buffer-merge API, scenegraph classification
#   metadata, occlusion thresholds, or compile scheduling.

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix);''',
    '''        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix,
            bool cacheViewInverse = false);

        bool hasCachedViewInverse() const { return mV322ViewInverseValid; }
        const osg::Matrixd& getCachedViewInverse() const { return mV322ViewInverse; }
        const osg::Vec3f& getCachedEyeWorld() const { return mV322EyeWorld; }''',
)

replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        osg::Matrixd mViewProjection;
        float mVPFloat[16] = {};''',
    '''        osg::Matrixd mViewProjection;
        osg::Matrixd mV322ViewInverse;
        osg::Vec3f mV322EyeWorld;
        bool mV322ViewInverseValid = false;
        float mVPFloat[16] = {};''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''    void OcclusionCuller::beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix)
    {''',
    '''    void OcclusionCuller::beginFrame(
        const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix, bool cacheViewInverse)
    {''',
)

replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''        mViewProjection = viewMatrix * projectionMatrix;

        const double* vpDouble = mViewProjection.ptr();''',
    '''        mV322ViewInverseValid = false;
        if (cacheViewInverse)
        {
            mV322ViewInverseValid = mV322ViewInverse.invert(viewMatrix);
            if (mV322ViewInverseValid)
            {
                mV322EyeWorld = osg::Vec3f(static_cast<float>(mV322ViewInverse(3, 0)),
                    static_cast<float>(mV322ViewInverse(3, 1)), static_cast<float>(mV322ViewInverse(3, 2)));
            }
        }

        mViewProjection = viewMatrix * projectionMatrix;

        const double* vpDouble = mViewProjection.ptr();''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        bool mEnableInteriors;
        bool mIsInterior = false;''',
    '''        bool mEnableInteriors;
        bool mV322MsocHotPath = false;
        bool mIsInterior = false;''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mMaxDistanceSq;
        unsigned int mMaxTriangles;''',
    '''        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mMaxDistanceSq;
        unsigned int mMaxTriangles;
        bool mV322MsocHotPath = false;
        osg::Node* mV322PagedDataNode = nullptr;
        osg::ref_ptr<PagedOccluderData> mV322PagedData;''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''#include <algorithm>
#include <cmath>''',
    '''#include <algorithm>
#include <cmath>
#include <cstdlib>''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''    namespace
    {
        std::string_view getModelPathForNode''',
    '''    namespace
    {
        bool v322MsocHotPathEnabled()
        {
            const char* value = std::getenv("OPENMW_V322_CP1_MSOC_HOT_PATH");
            return value && value[0] == '1';
        }

        std::string_view getModelPathForNode''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        , mEnableInteriors(enableInteriors)
        , mStorage(storage)''',
    '''        , mEnableInteriors(enableInteriors)
        , mV322MsocHotPath(v322MsocHotPathEnabled())
        , mStorage(storage)''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix());''',
    '''        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix(), mV322MsocHotPath);''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        , mMaxDistanceSq(maxDistance * maxDistance)
        , mMaxTriangles(maxTriangles)
    {
    }''',
    '''        , mMaxDistanceSq(maxDistance * maxDistance)
        , mMaxTriangles(maxTriangles)
        , mV322MsocHotPath(v322MsocHotPathEnabled())
    {
    }''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        osg::Matrixd viewInverse;
        viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;

        PagedOccluderData* pagedData = nullptr;
        if (auto* udc = node->getUserDataContainer())
        {
            for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
            {
                if (auto* candidate = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                {
                    pagedData = candidate;
                    break;
                }
            }
        }''',
    '''        const bool useCachedView = mV322MsocHotPath && mCuller->hasCachedViewInverse();
        osg::Matrixd localViewInverse;
        const osg::Matrixd* viewInverse = nullptr;
        if (useCachedView)
            viewInverse = &mCuller->getCachedViewInverse();
        else
        {
            localViewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            viewInverse = &localViewInverse;
        }
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * (*viewInverse);

        PagedOccluderData* pagedData = nullptr;
        if (mV322MsocHotPath && mV322PagedDataNode == node && mV322PagedData)
            pagedData = mV322PagedData.get();
        else if (auto* udc = node->getUserDataContainer())
        {
            for (unsigned int i = 0; i < udc->getNumUserObjects(); ++i)
            {
                if (auto* candidate = dynamic_cast<PagedOccluderData*>(udc->getUserObject(i)))
                {
                    pagedData = candidate;
                    if (mV322MsocHotPath)
                    {
                        mV322PagedDataNode = node;
                        mV322PagedData = candidate;
                    }
                    break;
                }
            }
        }''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            const osg::Vec3f eyeWorld(viewInverse(3, 0), viewInverse(3, 1), viewInverse(3, 2));''',
    '''            const osg::Vec3f eyeWorld = useCachedView
                ? mCuller->getCachedEyeWorld()
                : osg::Vec3f(static_cast<float>((*viewInverse)(3, 0)),
                    static_cast<float>((*viewInverse)(3, 1)), static_cast<float>((*viewInverse)(3, 2)));''',
    expected=1,
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        osg::Matrixd viewInverse;
        viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;
        const osg::BoundingBox worldBounds = transformLocalBounds(mLocalBounds, modelToWorld);''',
    '''        osg::Matrixd localViewInverse;
        const osg::Matrixd* viewInverse = nullptr;
        if (mCuller->hasCachedViewInverse())
            viewInverse = &mCuller->getCachedViewInverse();
        else
        {
            localViewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
            viewInverse = &localViewInverse;
        }
        const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * (*viewInverse);
        const osg::BoundingBox worldBounds = transformLocalBounds(mLocalBounds, modelToWorld);''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat''',
    '''openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat / openmw-custom-v3.22-cp1-msoc-hotpath''',
)

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")

init_anchor = "$V321CP4ShadowCompat = '0'\n$V320EngineLuaFastPaths = '0'"
if launcher.count(init_anchor) != 1:
    raise RuntimeError("V3.22 CP1 launcher initialization anchor drifted")
launcher = launcher.replace(
    init_anchor,
    "$V321CP4ShadowCompat = '0'\n$V322CP1MsocHotPath = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)

menu131 = "Write-Host '131 = V3.21 CP4 full-body shadow and animation compatibility'"
if launcher.count(menu131) != 1:
    raise RuntimeError("V3.22 CP1 launcher lost final V3.21 Mode131 menu anchor")
launcher = launcher.replace(
    menu131,
    menu131
    + "\nWrite-Host '135 = V3.22 CP1 final V3.21 CP4 behavior control'"
    + "\nWrite-Host '136 = V3.22 CP1 MSOC hot-path cache (Mode135 + cached camera inverse/PagedData)'",
    1,
)

choice_line = next(
    (line for line in launcher.splitlines() if "Enter a listed mode (1-127 or 129-131)" in line), None
)
if not choice_line or ",'130','131'))" not in choice_line:
    raise RuntimeError("V3.22 CP1 launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Enter a listed mode (1-127 or 129-131)",
    "Enter a listed mode (1-127, 129-131, or 135-136)",
    1,
).replace(
    ",'130','131'))",
    ",'130','131','135','136'))",
    1,
)
launcher = launcher.replace(choice_line, new_choice, 1)

line131 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'131'"))
mode135_body = line131[line131.index("{") + 1 : line131.rindex("}")].strip()
if "$V321CP3FullBodyFirstPerson = '1'" not in mode135_body or "$V321CP4ShadowCompat = '1'" not in mode135_body:
    raise RuntimeError("V3.22 CP1 Mode131 source body drifted")
if "v321-cp4-shadow-compat" not in mode135_body:
    raise RuntimeError("V3.22 CP1 Mode131 experiment identity drifted")
mode135_body = mode135_body.replace("v321-cp4-shadow-compat", "v322-cp1-v321-control", 1)
mode135 = "        '135' { " + mode135_body + " }"
mode136_body = mode135_body.replace("v322-cp1-v321-control", "v322-cp1-msoc-hotpath", 1)
mode136 = "        '136' { " + mode136_body + "; $V322CP1MsocHotPath = '1' }"
launcher = launcher.replace(line131 + "\n", line131 + "\n" + mode135 + "\n" + mode136 + "\n", 1)

manifest_anchor = '    "v321_cp4_shadow_compat=$V321CP4ShadowCompat",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.22 CP1 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v322_cp1_msoc_hot_path=$V322CP1MsocHotPath",',
    1,
)

env_anchor = "    $env:OPENMW_V321_CP4_SHADOW_COMPAT = $V321CP4ShadowCompat"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.22 CP1 launcher environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V322_CP1_MSOC_HOT_PATH = $V322CP1MsocHotPath",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V321_CP4_SHADOW_COMPAT -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.22 CP1 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V322_CP1_MSOC_HOT_PATH -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.22 CP1 — MSOC hot-path efficiency
=====================================

V3.22 starts from the runtime-accepted V3.21 CP4 head. Modes 132-134 remain
unused because those numbers were involved in canceled/audit-only V3.21 work.
Mode 135 is the final V3.21 Mode131 behavior control. Mode 136 adds only the
first low-risk V3.22 CPU render/cull optimization pack.

CP1 does not change occlusion thresholds, occluder geometry, visibility
decisions, exact-active ObjectPaging Mode2 batching/shareState/posttransform,
CP2 fairness, FBFP behavior, shadow behavior, or compile scheduling. It does
not attach any new UserDataContainer/DummyObject classification metadata.

The first mechanism caches the inverse main-camera view matrix and eye position
once when the active MSOC frame begins. Paged-object and groundcover coarse
callbacks reuse that immutable per-frame transform instead of independently
inverting the same camera matrix dozens of times per frame. If inversion fails
or CP1 is off, callbacks execute the original local-inversion path.

The second mechanism keeps each PagedOccluderCallback's resolved
PagedOccluderData after its first lookup on a given node. A node-identity guard
forces a fresh lookup if a callback is ever shared with another node, preserving
the original semantics while removing repeated UserDataContainer scans and
dynamic_casts on the steady cull path.

This checkpoint intentionally excludes unverified OSG-frustum shortcuts, MOC
buffer merging, off-thread proxy construction, new scenegraph metadata, and
occluder-aggressiveness changes. Those require separate API/lifetime proof
before they are allowed into a build.

Validation is Mode135 versus Mode136 with the frozen current mod/save/settings
cohort, native 1920x1080, AA4, groundcover 1.0, shadow distance 4096, OSG
Automatic, and Framepacer disabled/nonbinding. Preserve PBR, shadows, water,
groundcover, doors, and V3.21 FBFP compatibility. Primary metrics are OSG cull,
rendering traversal, wall p95/p99/tails, MSOC effectiveness, and VRAM.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"],
    cwd=ROOT,
    check=True,
    stdout=subprocess.PIPE,
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"],
    cwd=ROOT,
    check=True,
    text=True,
    stdout=subprocess.PIPE,
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

scene_hpp = (ROOT / "components/sceneutil/occlusionculling.hpp").read_text(encoding="utf-8")
scene_cpp = (ROOT / "components/sceneutil/occlusionculling.cpp").read_text(encoding="utf-8")
render_hpp = (ROOT / "apps/openmw/mwrender/occlusionculling.hpp").read_text(encoding="utf-8")
render_cpp = (ROOT / "apps/openmw/mwrender/occlusionculling.cpp").read_text(encoding="utf-8")
engine_cpp = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")
readme = readme_path.read_text(encoding="utf-8")

for marker in (
    "mV322ViewInverse",
    "mV322ViewInverseValid",
    "hasCachedViewInverse",
    "getCachedViewInverse",
    "getCachedEyeWorld",
):
    if marker not in scene_hpp + scene_cpp:
        raise RuntimeError(f"V3.22 CP1 cached camera transform incomplete: {marker}")

for marker in (
    "OPENMW_V322_CP1_MSOC_HOT_PATH",
    "mV322MsocHotPath",
    "mV322PagedDataNode",
    "mV322PagedData",
):
    if marker not in render_hpp + render_cpp:
        raise RuntimeError(f"V3.22 CP1 renderer hot-path marker missing: {marker}")

if "openmw-custom-v3.22-cp1-msoc-hotpath" not in engine_cpp:
    raise RuntimeError("V3.22 CP1 engine identity missing")

for marker in (
    "135 = V3.22 CP1 final V3.21 CP4 behavior control",
    "136 = V3.22 CP1 MSOC hot-path cache",
    "v322_cp1_msoc_hot_path=$V322CP1MsocHotPath",
    "OPENMW_V322_CP1_MSOC_HOT_PATH",
    "Enter a listed mode (1-127, 129-131, or 135-136)",
):
    if marker not in launcher:
        raise RuntimeError(f"V3.22 CP1 launcher marker missing: {marker}")

line135 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'135'"))
line136 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'136'"))
for line in (line135, line136):
    if "$V321CP3FullBodyFirstPerson = '1'" not in line or "$V321CP4ShadowCompat = '1'" not in line:
        raise RuntimeError("V3.22 causal modes lost accepted V3.21 CP3/CP4 foundation")
    if "$V321CP2Fairness = '1'" not in line:
        raise RuntimeError("V3.22 causal modes lost accepted V3.21 CP2 fairness foundation")
if "$V322CP1MsocHotPath = '1'" in line135:
    raise RuntimeError("V3.22 Mode135 control is contaminated by CP1")
if "$V322CP1MsocHotPath = '1'" not in line136:
    raise RuntimeError("V3.22 Mode136 does not enable CP1")
if any(line.lstrip().startswith(f"'{mode}'") for line in launcher.splitlines() for mode in ("132", "133", "134")):
    raise RuntimeError("V3.22 re-used canceled/audit-only V3.21 mode IDs 132-134")

for forbidden in (
    "markV322CompileClass",
    "OpenMW.V322CompileClass",
    "osg::DummyObject",
):
    if forbidden in render_cpp:
        raise RuntimeError(f"V3.22 CP1 introduced forbidden scenegraph metadata: {forbidden}")

for marker in (
    "exact-active ObjectPaging",
    "new UserDataContainer/DummyObject classification metadata",
    "Framepacer disabled/nonbinding",
):
    if marker not in readme:
        raise RuntimeError(f"V3.22 CP1 README scope lock missing: {marker}")

print("V3.22 CP1 MSOC hot-path layer applied; fail-closed invariants passed")
