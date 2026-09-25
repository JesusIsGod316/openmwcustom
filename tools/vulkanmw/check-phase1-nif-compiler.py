#!/usr/bin/env python3
"""VulkanMW Phase 1 direct-NIF semantic compiler source contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

NATIVE = [
    ROOT / "components/render/native/nifsemanticcompiler.hpp",
    ROOT / "components/render/native/nifsemanticcompiler.cpp",
]
TRANSLATOR = [
    ROOT / "components/nifrender/niftranslator.hpp",
    ROOT / "components/nifrender/niftranslator.cpp",
    ROOT / "components/nifrender/staticniftranslator.cpp",
]

SCENE_TOKENS = [
    re.compile(r"#\s*include\s*[<\"](?:osg|osgViewer|osgUtil|osgAnimation|osgParticle)/"),
    re.compile(r"#\s*include\s*[<\"]components/(?:sceneutil|nifosg)/"),
    re.compile(r"\bosg::(?:Node|Group|Geode|Drawable|Geometry|StateSet|NodeVisitor|Transform|MatrixTransform|Switch|Camera)\b"),
    re.compile(r"\bSceneUtil::"),
]

def fail(message: str) -> None:
    print(f"VulkanMW Phase 1 contract FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)

def main() -> int:
    for path in NATIVE + TRANSLATOR:
        if not path.is_file():
            fail(f"missing required source: {path.relative_to(ROOT)}")
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in SCENE_TOKENS:
            match = pattern.search(text)
            if match:
                line = text.count("\n", 0, match.start()) + 1
                fail(f"{path.relative_to(ROOT)}:{line} imports live OSG/SceneUtil scene semantics")

    cpp = NATIVE[1].read_text(encoding="utf-8")
    for needle in (
        "Nif::Reader",
        "reader.parse(mVfs.get(path))",
        "NifRender::translateStaticNif",
    ):
        if needle not in cpp:
            fail(f"native compiler no longer contains required direct-NIF step: {needle}")

    translator = (ROOT / "components/nifrender/niftranslator.cpp").read_text(encoding="utf-8")
    for needle in (
        "RenderCore::ModelNodeRecord",
        "RenderCore::MeshRecord",
        "RenderCore::SkinPayload",
        "RenderCore::MorphPayload",
    ):
        if needle not in translator:
            fail(f"neutral translator lost required semantic output: {needle}")

    print("VulkanMW Phase 1 direct-NIF semantic compiler contract PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
