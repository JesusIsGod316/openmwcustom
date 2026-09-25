#!/usr/bin/env python3
"""VulkanMW Phase 2 native static-world source contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

NATIVE_STATIC = [
    ROOT / "components/render/native/staticworldservice.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.hpp",
    ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp",
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
    ):
        if needle not in service:
            fail(f"native static-world service lost required ownership step: {needle}")

    source = code_only(
        (ROOT / "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp").read_text(encoding="utf-8")
    )
    for needle in (
        "glm::angleAxis(position.rot[2], glm::vec3(0.f, 0.f, -1.f))",
        "glm::angleAxis(position.rot[1], glm::vec3(0.f, -1.f, 0.f))",
        "glm::angleAxis(position.rot[0], glm::vec3(-1.f, 0.f, 0.f))",
        "RenderCore::StaticInstanceSource",
    ):
        if needle not in source:
            fail(f"OSG-free static placement source lost required semantic: {needle}")

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

    cmake = (ROOT / "apps/openmw/mwrender/v4engine-sources.cmake").read_text(encoding="utf-8")
    if "apps/openmw/mwrender/vulkanmw/staticworldsource.cpp" not in cmake:
        fail("production OpenMW target does not compile VulkanMW staticworldsource.cpp")

    print("VulkanMW Phase 2 native static-world contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
