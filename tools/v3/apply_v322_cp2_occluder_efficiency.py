import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.22 CP2 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.22 CP2 patched {rel} ({count} match(es))")


# CP2 innovation ladder. All modes retain the accepted V3.21 renderer topology and
# CP1 hot-path cache. CP2 changes only which already-eligible unpaged building
# proxies consume the existing MSOC triangle budget and, in the aggressive modes,
# extends eligibility to 300 radius without increasing proxy detail for the old
# 400+ population. Scene traversal order and visibility of the building itself are
# never changed by CP2.

replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        bool mEnableStaticOccluders;
        bool mV34BroadenOcclusion;
        unsigned int mMaxTriangles;''',
    '''        bool mEnableStaticOccluders;
        bool mV34BroadenOcclusion;
        int mV322Cp2OccluderMode = 0;
        unsigned int mMaxTriangles;''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        bool v322MsocHotPathEnabled()
        {
            const char* value = std::getenv("OPENMW_V322_CP1_MSOC_HOT_PATH");
            return value && value[0] == '1';
        }

        std::string_view getModelPathForNode''',
    '''        bool v322MsocHotPathEnabled()
        {
            const char* value = std::getenv("OPENMW_V322_CP1_MSOC_HOT_PATH");
            return value && value[0] == '1';
        }

        int v322Cp2OccluderMode()
        {
            const char* value = std::getenv("OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE");
            if (!value || value[1] != '\0' || value[0] < '1' || value[0] > '4')
                return 0;
            return value[0] - '0';
        }

        std::string_view getModelPathForNode''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        , mEnableStaticOccluders(enableStaticOccluders)
        , mV34BroadenOcclusion(v34BroadenOcclusion)
        , mMaxTriangles(maxTriangles)''',
    '''        , mEnableStaticOccluders(enableStaticOccluders)
        , mV34BroadenOcclusion(v34BroadenOcclusion)
        , mV322Cp2OccluderMode(v322Cp2OccluderMode())
        , mMaxTriangles(maxTriangles)''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        int meshRes = mOccluderMeshResolution;
        float radius = node->getBound().radius();
        if (radius > mOccluderMinRadius && mOccluderMinRadius > 0)
        {
            float scale = radius / mOccluderMinRadius;
            meshRes = std::clamp(
                static_cast<int>(mOccluderMeshResolution * scale), mOccluderMeshResolution, mOccluderMaxMeshResolution);
        }''',
    '''        int meshRes = mOccluderMeshResolution;
        float radius = node->getBound().radius();
        // Modes 3-4 lower eligibility to 300 but deliberately keep the final V3.21
        // 400-unit radius as the proxy-detail reference. This adds smaller useful
        // occluders without making the existing 400+ population more expensive.
        const float detailReferenceRadius
            = mV322Cp2OccluderMode >= 3 ? 400.0f : mOccluderMinRadius;
        if (radius > detailReferenceRadius && detailReferenceRadius > 0)
        {
            float scale = radius / detailReferenceRadius;
            meshRes = std::clamp(
                static_cast<int>(mOccluderMeshResolution * scale), mOccluderMeshResolution, mOccluderMaxMeshResolution);
        }''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        const unsigned int numChildren = node->getNumChildren();

        // Pass 1: Large objects — test against terrain depth, optionally rasterize as occluders''',
    '''        const unsigned int numChildren = node->getNumChildren();

        struct V322OccluderCandidate
        {
            const OccluderMesh* mesh = nullptr;
            float distanceSq = 0.0f;
            float utility = 0.0f;
            unsigned int triangles = 0;
        };
        std::vector<V322OccluderCandidate> v322Candidates;
        if (mV322Cp2OccluderMode > 0)
            v322Candidates.reserve(numChildren);

        // Pass 1: Large objects — test against terrain depth, optionally rasterize as occluders''',
)

old_raster = '''            // Rasterize as occluder if in range and camera is not inside the building.
            // Test against terrain-only buffer so other buildings don't prevent rasterization
            // of adjacent buildings (which would reduce culling coverage for Pass 2).
            bool v34RasterizedAsOccluder = false;
            if (mesh.aabb.valid() && mEnableStaticOccluders && !mesh.indices.empty()
                && mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
            {
                float distSq = (bs.center() - cv->getEyePoint()).length2();
                if (distSq < mOccluderMaxDistanceSq)
                {
                    osg::Vec3f center = mesh.aabb.center();
                    osg::Vec3f halfExtent
                        = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                        * mOccluderInsideThreshold;
                    osg::BoundingBox scaledBB;
                    scaledBB.expandBy(center - halfExtent);
                    scaledBB.expandBy(center + halfExtent);
                    if (!scaledBB.contains(cv->getEyePoint()))
                    {
                        unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
                        if (mMaxTriangles == 0
                            || mCuller->getNumBuildingTris() + newTris <= mMaxTriangles)
                        {
                            mCuller->rasterizeOccluder(mesh.vertices, mesh.indices);
                            mCuller->incrementBuildingOccluders(
                                newTris, static_cast<unsigned int>(mesh.vertices.size()));
                            v34RasterizedAsOccluder = true;
                        }
                    }
                }
            }
'''
new_raster = '''            // Rasterize as occluder if in range and camera is not inside the building.
            // Test against terrain-only buffer so other buildings don't prevent rasterization
            // of adjacent buildings (which would reduce culling coverage for Pass 2).
            bool v34RasterizedAsOccluder = false;
            if (mesh.aabb.valid() && mEnableStaticOccluders && !mesh.indices.empty()
                && mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
            {
                const float distSq = (bs.center() - cv->getEyePoint()).length2();
                if (distSq < mOccluderMaxDistanceSq)
                {
                    osg::Vec3f center = mesh.aabb.center();
                    osg::Vec3f halfExtent
                        = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                        * mOccluderInsideThreshold;
                    osg::BoundingBox scaledBB;
                    scaledBB.expandBy(center - halfExtent);
                    scaledBB.expandBy(center + halfExtent);
                    if (!scaledBB.contains(cv->getEyePoint()))
                    {
                        const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
                        if (mV322Cp2OccluderMode > 0)
                        {
                            const float radius = bs.radius();
                            const float radiusSq = std::max(radius * radius, 1.0f);
                            const float projectedUtility = radiusSq / std::max(distSq, radiusSq);
                            const float trianglePenalty = 1.0f + static_cast<float>(newTris) / 256.0f;
                            v322Candidates.push_back(
                                V322OccluderCandidate{ &mesh, distSq, projectedUtility / trianglePenalty, newTris });
                            // Treat a queued candidate as self-occlusion-sensitive even if the
                            // later ranked budget cannot fit it. CP2 never culls the building itself.
                            v34RasterizedAsOccluder = true;
                        }
                        else if (mMaxTriangles == 0
                            || mCuller->getNumBuildingTris() + newTris <= mMaxTriangles)
                        {
                            mCuller->rasterizeOccluder(mesh.vertices, mesh.indices);
                            mCuller->incrementBuildingOccluders(
                                newTris, static_cast<unsigned int>(mesh.vertices.size()));
                            v34RasterizedAsOccluder = true;
                        }
                    }
                }
            }
'''
replace_exact("apps/openmw/mwrender/occlusionculling.cpp", old_raster, new_raster)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            child->accept(*cv);
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)''',
    '''            child->accept(*cv);
        }

        if (mV322Cp2OccluderMode > 0 && !v322Candidates.empty())
        {
            if (mV322Cp2OccluderMode == 1)
            {
                // Mode 1: pure front-to-back budget consumption. This improves depth
                // usefulness without changing the eligibility population.
                std::stable_sort(v322Candidates.begin(), v322Candidates.end(),
                    [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                        return lhs.distanceSq < rhs.distanceSq;
                    });
            }
            else
            {
                // Modes 2-4: approximate projected coverage per raster triangle.
                // Ties favor the nearer proxy so useful near depth arrives first.
                std::stable_sort(v322Candidates.begin(), v322Candidates.end(),
                    [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                        if (lhs.utility != rhs.utility)
                            return lhs.utility > rhs.utility;
                        return lhs.distanceSq < rhs.distanceSq;
                    });
            }

            for (const auto& candidate : v322Candidates)
            {
                if (!candidate.mesh)
                    continue;
                if (mMaxTriangles > 0
                    && mCuller->getNumBuildingTris() + candidate.triangles > mMaxTriangles)
                    continue;

                // Mode 4 is intentionally aggressive but visibility-safe: if terrain or
                // an earlier ranked proxy already fully hides this candidate AABB, omit
                // only its redundant raster work. The building's scene traversal already
                // happened above and is never culled by this decision.
                if (mV322Cp2OccluderMode >= 4 && !mCuller->testVisibleAABB(candidate.mesh->aabb))
                    continue;

                mCuller->rasterizeOccluder(candidate.mesh->vertices, candidate.mesh->indices);
                mCuller->incrementBuildingOccluders(
                    candidate.triangles, static_cast<unsigned int>(candidate.mesh->vertices.size()));
            }
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat / openmw-custom-v3.22-cp1-msoc-hotpath''',
    '''openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat / openmw-custom-v3.22-cp1-msoc-hotpath / openmw-custom-v3.22-cp2-occluder-efficiency''',
)

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")

init_anchor = "$V322CP1MsocHotPath = '0'\n$V320EngineLuaFastPaths = '0'"
if launcher.count(init_anchor) != 1:
    raise RuntimeError("V3.22 CP2 launcher initialization anchor drifted")
launcher = launcher.replace(
    init_anchor,
    "$V322CP1MsocHotPath = '0'\n$V322CP2OccluderMode = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)

menu136 = "Write-Host '136 = V3.22 CP1 MSOC hot-path cache (Mode135 + cached camera inverse/PagedData)'"
if launcher.count(menu136) != 1:
    raise RuntimeError("V3.22 CP2 launcher lost CP1 Mode136 menu anchor")
launcher = launcher.replace(
    menu136,
    menu136
    + "\nWrite-Host '137 = V3.22 CP2 front-to-back occluder budget (400 radius)'"
    + "\nWrite-Host '138 = V3.22 CP2 utility-ranked occluder budget (400 radius)'"
    + "\nWrite-Host '139 = V3.22 CP2 utility-ranked clean 300-radius eligibility'"
    + "\nWrite-Host '140 = V3.22 CP2 aggressive 300-radius + redundant-raster suppression'",
    1,
)

choice_line = next(
    (line for line in launcher.splitlines() if "Enter a listed mode (1-127, 129-131, or 135-136)" in line), None
)
if not choice_line or ",'135','136'))" not in choice_line:
    raise RuntimeError("V3.22 CP2 launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Enter a listed mode (1-127, 129-131, or 135-136)",
    "Enter a listed mode (1-127, 129-131, or 135-140)",
    1,
).replace(
    ",'135','136'))",
    ",'135','136','137','138','139','140'))",
    1,
)
launcher = launcher.replace(choice_line, new_choice, 1)

line136 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'136'"))
mode136_body = line136[line136.index("{") + 1 : line136.rindex("}")].strip()
if "$V322CP1MsocHotPath = '1'" not in mode136_body or "v322-cp1-msoc-hotpath" not in mode136_body:
    raise RuntimeError("V3.22 CP2 Mode136 source body drifted")

def make_mode(number, experiment, cp2_mode, lower_radius=False):
    body = mode136_body.replace("v322-cp1-msoc-hotpath", experiment, 1)
    body += f"; $V322CP2OccluderMode = '{cp2_mode}'"
    if lower_radius:
        body += "; $OccluderMinRadius = '300'"
    return f"        '{number}' {{ {body} }}"

mode137 = make_mode(137, "v322-cp2-front-to-back-400", 1)
mode138 = make_mode(138, "v322-cp2-utility-400", 2)
mode139 = make_mode(139, "v322-cp2-utility-300", 3, True)
mode140 = make_mode(140, "v322-cp2-utility-300-redundant-skip", 4, True)
launcher = launcher.replace(
    line136 + "\n", line136 + "\n" + mode137 + "\n" + mode138 + "\n" + mode139 + "\n" + mode140 + "\n", 1
)

manifest_anchor = '    "v322_cp1_msoc_hot_path=$V322CP1MsocHotPath",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.22 CP2 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v322_cp2_occluder_efficiency_mode=$V322CP2OccluderMode",',
    1,
)

env_anchor = "    $env:OPENMW_V322_CP1_MSOC_HOT_PATH = $V322CP1MsocHotPath"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.22 CP2 launcher environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE = $V322CP2OccluderMode",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V322_CP1_MSOC_HOT_PATH -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.22 CP2 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.22 CP2 — ranked occluder efficiency ladder
==============================================

CP2 deliberately uses one executable for an innovation ladder with immediate
fallback. Mode 136 remains the CP1 control. Modes 137-140 all retain CP1 and the
final V3.21 CP2/CP3/CP4 foundation; only unpaged building-proxy budget policy
changes.

137: same 400-radius population, but eligible proxies consume the existing
     30000-triangle budget front-to-back instead of arbitrary cell-child order.
138: same 400-radius population, sorted by approximate projected coverage per
     raster triangle, with near-distance tie breaking.
139: Mode138 plus a clean 300-radius eligibility test. Proxy-detail scaling keeps
     400 units as its reference, so lowering eligibility does not inflate the
     triangle complexity of the previously eligible 400+ population. Distance
     stays 6144 and the global cap stays 30000.
140: Mode139 plus redundant-raster suppression. Before a ranked proxy is
     rasterized, the already-built full MSOC buffer may prove its AABB fully
     hidden by terrain or an earlier proxy. Only raster work is skipped; the
     building itself was already traversed and is never culled by this decision.

All modes preserve child traversal order, ObjectPaging Mode2/shareState/
posttransform topology, PBR/shadow/FBFP semantics, door handling, terrain-only
cell rejection, camera-inside exclusion, and the existing global triangle cap.
The rejected historical 250-radius / 8192-distance / 45000-triangle broadened
configuration is not revived.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source.patch").open("w", encoding="utf-8", newline="\n"),
)
subprocess.run(
    ["git", "diff", "--stat", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source-stat.txt").open("w", encoding="utf-8", newline="\n"),
)
print("V3.22 CP2 ranked occluder-efficiency layer applied")
