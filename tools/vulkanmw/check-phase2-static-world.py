#!/usr/bin/env python3
"""VulkanMW Phase 2 native static-world source contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

NATIVE_STATIC = [
    ROOT / "components/render/native/staticworldservice.hpp",
    ROOT / "components/render/native/terrainworldservice.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/referenceplacement.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/groundcoversource.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/groundcoversource.cpp",
]

FORBIDDEN = [
    re.compile(r"#\s*include\s*[<\"](?:osg|osgViewer|osgUtil|osgAnimation|osgParticle)/"),
    re.compile(r"#\s*include\s*[<\"]components/(?:sceneutil|nifosg)/"),
    re.compile(r"\bosg::"),
    re.compile(r"\bSceneUtil::"),
    re.compile(r"\bNifOsg::"),
    re.compile(r"\bMisc::Convert::"),
]


def fail(message: str) -> None:
    print(f"VulkanMW Phase 2 static-world contract FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def main() -> int:
    for path in NATIVE_STATIC:
        if not path.is_file():
            fail(f"missing required Phase 2 source: {path.relative_to(ROOT)}")
        code = code_only(path.read_text(encoding="utf-8", errors="replace"))
        for pattern in FORBIDDEN:
            match = pattern.search(code)
            if match:
                line = code.count("\n", 0, match.start()) + 1
                fail(f"{path.relative_to(ROOT)}:{line} imports legacy OSG/SceneUtil static rendering semantics")

    service = (ROOT / "components/render/native/staticworldservice.hpp").read_text(encoding="utf-8")
    for needle in (
        "class StaticWorldService",
        "mCells.addCell",
        "mPopulations.addCell",
        "mCells.upsertStaticInstance",
        "mPopulations.upsert",
        "mCells.removeInstance",
        "mPopulations.remove",
        "activatePopulationCell",
        "deactivatePopulationCell",
        "upsertPopulation",
    ):
        if needle not in service:
            fail(f"native static-world service lost required ownership step: {needle}")

    source = code_only(
        (ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp").read_text(encoding="utf-8")
    )
    if "RenderCore::StaticInstanceSource" not in source or "makeReferenceRotation(position)" not in source:
        fail("OSG-free static placement source lost native placement publication")

    placement = code_only(
        (ROOT / "apps/openmw/mwrender/vulkanmw/referenceplacement.hpp").read_text(encoding="utf-8")
    )
    for needle in (
        "glm::angleAxis(position.rot[2], glm::vec3(0.f, 0.f, -1.f))",
        "glm::angleAxis(position.rot[1], glm::vec3(0.f, -1.f, 0.f))",
        "glm::angleAxis(position.rot[0], glm::vec3(-1.f, 0.f, 0.f))",
    ):
        if needle not in placement:
            fail(f"OSG-free reference placement lost required rotation semantic: {needle}")

    groundcover = code_only(
        (ROOT / "apps/openmw/mwrender/vulkanmw/groundcoversource.cpp").read_text(encoding="utf-8")
    )
    for needle in (
        "groundcover.collectInstances(1.0f",
        "assets.resolve(modelPath)",
        "makeReferenceRotation(entry.mPos)",
        "StaticPopulationInstanceSource",
    ):
        if needle not in groundcover:
            fail(f"native groundcover source lost required semantic: {needle}")

    lifecycle = code_only(
        (ROOT / "apps/openmw/mwrender/v4scenerenderlifecycle.cpp").read_text(encoding="utf-8")
    )
    for needle in (
        "VulkanMW::makeStaticWorldCellSource(cell)",
        "mNativeStaticWorld->activateCell",
        "mNativeStaticWorld->deactivateCell",
        "mNativeStaticWorld->upsertStatic",
        "mNativeStaticWorld->removeStatic",
    ):
        if needle not in lifecycle:
            fail(f"production scene lifecycle bypasses native static-world route: {needle}")

    for forbidden in (
        "makeV4StaticInstanceSource",
        "mSession->cells().upsertStaticInstance",
        "mSession->populations().addCell",
        "mSession->populations().upsert",
        "mSession->populations().removeCell",
    ):
        if forbidden in lifecycle:
            fail(f"production static lifecycle still owns legacy/direct mutation: {forbidden}")

    legacy_header = (ROOT / "apps/openmw/mwrender/v4semanticsource.hpp").read_text(encoding="utf-8")
    legacy_cpp = (ROOT / "apps/openmw/mwrender/v4semanticsource.cpp").read_text(encoding="utf-8")
    if "makeV4StaticInstanceSource" in legacy_header or "makeV4StaticInstanceSource" in legacy_cpp:
        fail("legacy OSG static placement adapter still exists")

    terrain_service = (ROOT / "components/render/native/terrainworldservice.hpp").read_text(encoding="utf-8")
    for needle in (
        "class TerrainWorldService",
        "TerrainPreparationService",
        "TerrainResidencyPlanner",
        "mTerrain.synchronize",
        "updateResidency",
        "stopBackgroundPreparation",
    ):
        if needle not in terrain_service:
            fail(f"native terrain service lost required ownership step: {needle}")

    terrain_adapter = code_only(
        (ROOT / "apps/openmw/mwrender/v4terrainsource.cpp").read_text(encoding="utf-8")
    )
    for forbidden in (
        "osg::Node",
        "osg::Group",
        "osg::Geode",
        "osg::Geometry",
        "osg::StateSet",
        "osg::NodeVisitor",
        "SceneUtil::",
        "NifOsg::",
    ):
        if forbidden in terrain_adapter:
            fail(f"LAND extractor regressed into OSG scene ownership: {forbidden}")

    bridge = code_only(
        (ROOT / "apps/openmw/mwrender/v4enginerenderbridge.cpp").read_text(encoding="utf-8")
    )
    for needle in (
        "VulkanMW::makeGroundcoverPopulationSource",
        "mNativeStaticWorld->activatePopulationCell",
        "mNativeStaticWorld->upsertPopulation",
        "mNativeStaticWorld->deactivatePopulationCell",
    ):
        if needle not in bridge:
            fail(f"production groundcover route bypasses native semantic service: {needle}")
    for forbidden in (
        "makeOsgQuat(entry.mPos)",
        "mSession->populations().addCell",
        "mSession->populations().upsert",
        "mSession->populations().removeCell",
        "mTerrainPreparation",
        "mTerrainResidencyPlanner",
        "mPendingTerrainPublication",
        "mTerrain->synchronize",
    ):
        if forbidden in bridge:
            fail(f"production terrain/groundcover bridge still owns legacy/direct mutation: {forbidden}")

    for needle in (
        "mNativeTerrain->updateResidency",
        "mNativeTerrain->synchronize",
        "mNativeTerrain->clear",
        "mNativeTerrain->stopBackgroundPreparation",
    ):
        if needle not in bridge:
            fail(f"production terrain route bypasses native terrain service: {needle}")

    cmake = (ROOT / "apps/openmw/mwrender/v4engine-sources.cmake").read_text(encoding="utf-8")
    for source_file in (
        "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp",
        "apps/openmw/mwrender/vulkanmw/groundcoversource.cpp",
    ):
        if source_file not in cmake:
            fail(f"production OpenMW target does not compile {source_file}")

    print("VulkanMW Phase 2 native static-world/groundcover contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
