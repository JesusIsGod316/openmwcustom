import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    if chr(0) in new:
        raise RuntimeError(f"{rel}: V3.22 CP2 eligibility replacement contains an embedded NUL")
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} CP2 eligibility match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.22 CP2 eligibility refinement patched {rel} ({count} match(es))")


# Decouple the experimental 300-unit occluder eligibility floor from the proven
# 400-unit visibility-classification boundary. Objects in [300, 400) retain the
# control path's full-buffer visibility decision and original Pass-2 traversal
# order. Only after all such owner visibility decisions are recorded may their
# visible proxies enrich depth for <300 objects.

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
for mode in ("139", "140"):
    lines = launcher.splitlines()
    matches = [i for i, line in enumerate(lines) if line.lstrip().startswith(f"'{mode}'")]
    if len(matches) != 1:
        raise RuntimeError(f"V3.22 CP2 eligibility expected one Mode{mode} line, found {len(matches)}")
    idx = matches[0]
    token = "; $OccluderMinRadius = '300'"
    if token not in lines[idx]:
        raise RuntimeError(f"V3.22 CP2 Mode{mode} lost the temporary 300-radius launcher token")
    lines[idx] = lines[idx].replace(token, "", 1)
    launcher = "\n".join(lines) + "\n"
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

anchor = '''        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)
        for (unsigned int i = 0; i < numChildren; ++i)'''
mid_phase = '''        // Modes 3-4 decouple 300-unit occluder eligibility from the proven
        // 400-unit visibility-classification boundary. First record visibility for
        // every [300, 400) owner against the same terrain + 400+ proxy buffer that
        // the control path would use. No owner is traversed here: Pass 2 retains the
        // original child traversal order and consumes the recorded result later.
        std::vector<unsigned char> v322MidVisibility;
        if (mV322Cp2OccluderMode >= 3)
        {
            constexpr float v322EligibilityRadius = 300.0f;
            v322MidVisibility.assign(numChildren, 0);
            std::vector<V322OccluderCandidate> v322MidCandidates;
            v322MidCandidates.reserve(numChildren);

            for (unsigned int i = 0; i < numChildren; ++i)
            {
                osg::Node* child = node->getChild(i);
                const osg::BoundingSphere& bs = child->getBound();
                if (!bs.valid() || bs.radius() < v322EligibilityRadius || bs.radius() >= mOccluderMinRadius)
                    continue;

                bool skipOcclusion = false;
                child->getUserValue("skipOcclusion", skipOcclusion);

                osg::BoundingBox childBB;
                childBB.expandBy(bs);
                if (!skipOcclusion && !mCuller->testVisibleAABB(childBB))
                    continue;

                // Store the control-equivalent owner result before this object can
                // contribute any depth itself. Doors and other explicit exclusions
                // remain visible but are never admitted as CP2 occluders.
                v322MidVisibility[i] = 1;
                if (skipOcclusion || !mEnableStaticOccluders)
                    continue;

                const OccluderMesh& mesh = getOccluderMesh(child);
                if (!mesh.aabb.valid() || mesh.indices.empty() || !mCuller->testVisibleAABBTerrainOnly(mesh.aabb))
                    continue;

                const float distSq = (bs.center() - cv->getEyePoint()).length2();
                if (distSq >= mOccluderMaxDistanceSq)
                    continue;

                osg::Vec3f center = mesh.aabb.center();
                osg::Vec3f halfExtent
                    = (osg::Vec3f(mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax()) - center)
                    * mOccluderInsideThreshold;
                osg::BoundingBox scaledBB;
                scaledBB.expandBy(center - halfExtent);
                scaledBB.expandBy(center + halfExtent);
                if (scaledBB.contains(cv->getEyePoint()))
                    continue;

                const unsigned int newTris = static_cast<unsigned int>(mesh.indices.size() / 3);
                const float radius = bs.radius();
                const float radiusSq = std::max(radius * radius, 1.0f);
                const float projectedUtility = radiusSq / std::max(distSq, radiusSq);
                const float trianglePenalty = 1.0f + static_cast<float>(newTris) / 256.0f;
                v322MidCandidates.push_back(
                    V322OccluderCandidate{ &mesh, distSq, projectedUtility / trianglePenalty, newTris });
            }

            std::sort(v322MidCandidates.begin(), v322MidCandidates.end(),
                [](const V322OccluderCandidate& lhs, const V322OccluderCandidate& rhs) {
                    if (lhs.utility != rhs.utility)
                        return lhs.utility > rhs.utility;
                    return lhs.distanceSq < rhs.distanceSq;
                });

            for (const auto& candidate : v322MidCandidates)
            {
                if (!candidate.mesh)
                    continue;
                if (mMaxTriangles > 0
                    && mCuller->getNumBuildingTris() + candidate.triangles > mMaxTriangles)
                    continue;

                // Mode 4 may suppress only redundant proxy raster work. Owner visibility
                // was already resolved above without this proxy in depth, and actual owner
                // traversal remains deferred to Pass 2 in original child order.
                if (mV322Cp2OccluderMode >= 4 && !mCuller->testVisibleAABB(candidate.mesh->aabb))
                    continue;

                mCuller->rasterizeOccluder(candidate.mesh->vertices, candidate.mesh->indices);
                mCuller->incrementBuildingOccluders(
                    candidate.triangles, static_cast<unsigned int>(candidate.mesh->vertices.size()));
            }
        }

        // Pass 2: Small objects — test against enriched depth buffer (terrain + buildings)
        for (unsigned int i = 0; i < numChildren; ++i)'''
replace_exact("apps/openmw/mwrender/occlusionculling.cpp", anchor, mid_phase)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            if (bs.radius() >= mOccluderMinRadius)
                continue; // Already handled in pass 1''',
    '''            if (bs.radius() >= mOccluderMinRadius)
                continue; // Already handled in pass 1

            if (mV322Cp2OccluderMode >= 3 && bs.radius() >= 300.0f)
            {
                // The visibility decision was recorded before any [300,400) proxy
                // entered the buffer, preventing self-occlusion while preserving
                // this original Pass-2 traversal position.
                if (i < v322MidVisibility.size() && v322MidVisibility[i] != 0)
                    child->accept(*cv);
                continue;
            }''',
)

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''

CP2 eligibility-decoupling correction
-------------------------------------
Modes 139-140 do NOT lower the shared Camera/occlusion occluder min radius
setting. The established 400-unit boundary still decides which objects use the
large-owner path versus the normal full-buffer small-object visibility path.
CP2 separately considers 300-399-radius objects as potential occluders. Their
visibility result is recorded first against terrain + 400+ proxies, with their
own proxy absent. Visible mid-size proxies are then ranked/rasterized, but owner
traversal remains in the original Pass2 child order and consumes the recorded
visibility result without a second full-buffer test. This prevents self-
occlusion, preserves render traversal ordering, and avoids turning the 300-radius
experiment into a visibility-classification change.
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
print("V3.22 CP2 eligibility decoupling applied: 300 occluder floor / 400 visibility boundary / Pass2 order preserved")
