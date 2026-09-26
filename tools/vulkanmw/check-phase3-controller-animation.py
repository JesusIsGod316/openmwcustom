#!/usr/bin/env python3
"""VulkanMW Phase 3 native controller/animation source contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

PROGRAM_HPP = ROOT / "components/render/native/nifcontrollerprogram.hpp"
PROGRAM_CPP = ROOT / "components/render/native/nifcontrollerprogram.cpp"
SEMANTIC_HPP = ROOT / "components/render/native/nifsemanticcompiler.hpp"
SEMANTIC_CPP = ROOT / "components/render/native/nifsemanticcompiler.cpp"
ASSET_HPP = ROOT / "components/render/native/nifassetservice.hpp"
ASSET_CPP = ROOT / "components/render/native/nifassetservice.cpp"
RUNTIME_SOURCES = ROOT / "components/render/backend/vsg/runtime-sources.cmake"

FORBIDDEN = (
    re.compile(r"#\s*include\s*[<\"](?:osg|osgAnimation|osgParticle|osgUtil|osgViewer)/"),
    re.compile(r"\bosg::"),
    re.compile(r"\bSceneUtil::"),
    re.compile(r"\bNifOsg::"),
)


def fail(message: str) -> None:
    print(f"VulkanMW Phase 3 controller contract FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def required(path: pathlib.Path, needles: tuple[str, ...]) -> None:
    if not path.is_file():
        fail(f"missing required file: {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for needle in needles:
        if needle not in text:
            fail(f"{path.relative_to(ROOT)} lost required Phase 3 contract: {needle}")


def main() -> int:
    for path in (PROGRAM_HPP, PROGRAM_CPP):
        if not path.is_file():
            fail(f"missing native controller source: {path.relative_to(ROOT)}")
        code = code_only(path.read_text(encoding="utf-8", errors="replace"))
        for pattern in FORBIDDEN:
            if pattern.search(code):
                fail(f"{path.relative_to(ROOT)} imports legacy scene/controller runtime semantics")

    required(PROGRAM_HPP, (
        "struct NifControllerProgram",
        "struct TransformControllerProgram",
        "struct VisibilityControllerProgram",
        "struct MorphControllerProgram",
        "ControllerTiming",
        "evaluateAutoplay",
        "class NifControllerCompiler",
    ))
    required(PROGRAM_CPP, (
        "NifControllerCompiler::compile",
        "bundle.model.nodes[i].sourceRecordId",
        "RC_NiKeyframeController",
        "RC_NiVisController",
        "RC_NiGeomMorpherController",
        "ControllerExtrapolation::Cycle",
        "ControllerExtrapolation::Reverse",
        "glm::slerp",
    ))
    required(SEMANTIC_HPP, (
        "NifControllerProgram controllers;",
        "controllers.valid()",
    ))
    required(SEMANTIC_CPP, (
        "result.controllers = NifControllerCompiler::compile(file, result.bundle);",
    ))
    required(ASSET_HPP, (
        "std::shared_ptr<const NifControllerProgram> controllers;",
        "struct SourceMetadata",
    ))
    required(ASSET_CPP, (
        "std::make_shared<const NifControllerProgram>",
        "metadata->second.controllers",
    ))
    required(RUNTIME_SOURCES, (
        "components/render/native/nifcontrollerprogram.cpp",
    ))

    bridge = code_only((ROOT / "apps/openmw/mwrender/v4enginerenderbridge.cpp").read_text(encoding="utf-8"))
    if "captureDynamicFrameState" not in bridge:
        fail("transitional dynamic-frame bridge unexpectedly disappeared before Phase 3 production replacement")
    if "MorphCollector" not in bridge:
        fail("Phase 3A checkpoint no longer matches the known transitional morph-capture baseline")

    print("VulkanMW Phase 3 native controller/animation contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
